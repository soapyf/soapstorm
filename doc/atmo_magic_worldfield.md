# Atmo Magic: the shared world field

> **Implementation status (2026-09-11).** The world field has **no capture and no
> store of its own**. Its geometry is the census navmesh's own rasterization: every
> band build already reads its `rcHeightfield` out as a `SSNavMesh::SpanSheet` (a
> 64 x 64 grid of 0.25 m cells over the band's 16 m column, up to six solid spans a
> cell), and those sheets now stay on the navmesh's bands for the band's whole life.
> The world field is the classification over them. Per region it snapshots the band
> sheets — `shared_ptr` copies, no geometry copied — and one General-queue job
> materialises the region's cell grid, walks the air, and bakes the acoustic probes.
> The finished grid is swapped in whole under a serial gate.
>
> **Deleted in this round:** the staged depth-peel capture (`advanceBuild`,
> `capturePass`, `ensureBands`, `applyBand`, `applyRefine`, `foldSpans`,
> `finalizeSpans`, `commitBuild`, the Z-bisection and XY-quadtree worklist, the
> `CaptureNode`/`Build` machinery, the band scratch arrays, the `LLRenderTarget` and
> its async readback), the incremental `navSpans` splice and its `spanInsert` into a
> live store, the dirty-rectangle re-peel path, and the settings that only fed them
> (`SSWorldFieldFromNavMesh`, `SSWorldFieldBand`, `SSWorldFieldCeiling`,
> `SSWorldFieldMaxAge`). About 1,700 lines net.
>
> **Added:** `ssworldfieldcore.h` (the pure core: `spanInsert`, `buildGrid`,
> `gapIndexAt`, `classify`), the geometric outdoors rule, the walk-down point query,
> the survey mask, `snapshotGrid` for worker consumers, `SSWorldFieldOpenAngle` and
> `SSWorldFieldGroundReach`. Pinned by 24 cases across
> `V:\Scratch\atmo\tests\worldfieldcore{,_adv,_cost}.cpp`.
>
> **Review round, 2026-09-11.** Two reviews; the blocking findings are fixed here.
> A partial sheet set published a confidently-OUTDOORS region (now the survey mask); a
> band that built while the field was off held no sheet for ever (now
> `SSNavMesh::resheet` on the off-to-on edge); `spanInsert` could leave the span list
> overlapping and unsorted for a pillar through a ceiling (now cascading merges, and
> the pre-existing shipping bug it was copied from is fixed with it); the threading
> invariant as written was false (now stated as written-only-from-the-main-thread);
> and the wind flow workstream had no legal way to read the grid off-thread (now
> `snapshotGrid` plus a public `SSWorldFieldCore::buildGrid`). The covered distance is
> now a shortest-path walk rather than the widest path's length, `SHELTER_MULT` is 2
> rather than 4, `SSNavMesh`/`SSWorldFieldShapes` default on, and the classification's
> scratch is down from 130 MB to a measured 24.7 MB with the walk 183 ms to 150 ms.
>
> **Second review round, same day.** The re-review confirmed those landed and found
> three defects the fix round itself introduced, all now fixed. The survey mask made an
> unsurveyed neighbour block the reachability walk, which is right, but then labelled
> what it sealed off INTERIOR, which is the original bug with its sign flipped - a
> doubt flood now marks those pockets UNKNOWN and the field claims INTERIOR only where
> it has seen the walls. The budget was carried in decimetres, and `toDm(0.25)` is 3,
> so the shipping cell spent 0.3 m per 0.25 m of travel: reach was 17% short and
> distances 20% long. It is counted in whole cell steps now. And the acoustic bake
> never received the mask, so the anchor spiral could land on unsurveyed void and
> fabricate a probe with saturated walls and a long RT60.
>
> This document supersedes the capture-service design of 2026-09-01, which is kept
> below under **Archive** because two of its verdicts (Design F, physics-sweep
> capture; Design H, analytic edge refinement) are still live decisions, and the
> adversarial reviews are the record of why the shipped shape is the shipped shape.
> It in turn superseded `doc/archive/atmo_magic_worldfield.md`.

## Why the store went away

The 2026-09-01 design was built around a premise that stopped being true: that the
world field had to *capture* the world itself, because nothing else did. By
2026-09-09 the census navmesh did. It rasterises the declared-shape census into
16 m columns, splits each column into height bands wherever an air gap separates
the geometry, and runs Recast over each band on a worker. A Recast heightfield is
per-column solid spans — which is the world field's data structure, arrived at from
the other direction and produced for free.

The interim arrangement (`SSWorldFieldFromNavMesh`, default on since 2026-09-09)
kept both: the navmesh pushed each published band's sheet at the field, and the
field spliced it into a long-lived per-region span store — cutting the band's
z-range out of every column it covered, re-inserting, folding terrain to the world
floor, and bumping a serial once the navmesh went quiet. That arrangement had four
costs worth naming, because they are what this round removes:

1. **Two sources of truth.** The spans lived in the field's store; the geometry that
   produced them lived on the navmesh's bands. Nothing kept them in step except the
   splice, and the splice was the most delicate code in the file — it had to
   reconstruct "what this band used to say" by z-range subtraction.
2. **A store that was never consistent.** Sheets landed one band at a time over
   several seconds. Readers saw a half-fed region and could not tell. The serial
   moved only at settle, but the *spans* changed continuously underneath it.
3. **A deep copy per classification.** The flood snapshotted the whole span store
   (about 48 MB at 0.25 m over a 256 m region) so the walk would not race the next
   splice.
4. **A capture that was dead code.** The depth-peel path still existed, still
   compiled, still owned a render target and a readback worker, and could still be
   switched back on into a state nothing had exercised in weeks.

The fix is to stop copying. The sheets are immutable once published — a rebuilt band
swaps in a whole new one — so a reader that holds a `shared_ptr` to a sheet is safe
on any thread for as long as it holds it. Snapshotting a region is therefore a
vector of `shared_ptr` copies, and the classification job can do all of its work,
including materialising the dense grid, on the worker.

## The model

### Cells, and why 0.25 m

The navmesh's spatial structure is (column, band, layer). Two of those three are the
right anchor for classification and one is not:

- **Column** (16 m, keyed in a frame pinned at init so a region crossing never moves
  a key) — yes. This is the unit the navmesh rebuilds.
- **Band** (a storey-or-more of a column, bounded by air gaps of at least
  `SSNavMeshBandGap`) — yes. A band boundary is in open air by construction, which
  is what makes bands safe to merge vertically without inventing a floor.
- **Layer** (a DetourTileCache layer, one walkable surface within a band) — **no**.
  Layers are floors. A wall with no floor beside it produces no layer, and a wall
  with no floor is exactly what ray occlusion and the enclosure walk must see.
  The sheet is read from the `rcHeightfield` *before* layer partitioning, so it has
  every solid span including walls, railings and ceilings.

The cell resolution is **0.25 m**, which is the sheet's own cell (`SpanSheet::RES`
64 over `TILE_M` 16). Three reasons, in order of weight:

1. **It is free.** Any other choice is a resampling step, and resampling spans is
   lossy in the direction that matters — unioning two cells merges a doorway into
   the wall beside it.
2. **It is what `traceSolid`'s accuracy is measured at today.** Decision 3 of this
   round is that ray occlusion must not get worse. Keeping the cell keeps it exact.
3. **It resolves the openings the classification is about.** A door is 0.8–1.2 m and
   a wall is 0.1–0.3 m. At 0.25 m a door is 3–5 cells of gap and a wall is 1–2 cells
   of solid; the reach walk can tell them apart. At 0.5 m it cannot reliably, and
   the failure mode is a sealed room reading as sheltered, which is the worst
   direction to be wrong in.

The navmesh's own cells are 0.125 m. That is finer than classification needs — it
exists so Detour can represent thin ledges and stair treads — and `extractSpanSheet`
already unions two-by-two down to 0.25 m on the way out. `SSWorldFieldCell` remains
as a coarsening dial for anyone who wants the memory back; at any value above
0.25 m the grid unions whole sheet cells, with the leak-through-thin-walls cost
stated in the setting's own comment.

### The grid

A region's grid is region-anchored, `mRes x mRes` cells at `mCell` metres, and for
each cell up to `SS_WF_MAX_SPANS` (6, in lockstep with `SpanSheet::SPANS`) solid
`[bottom, top]` spans with `SSRainShadowMap::SURF_*` flags. Layout is
`[k * res * res + col]`, `col = y * res + x` — one contiguous plane per span slot,
which is the layout `SSAcoustic::Snap` already walks, so the acoustic core needed no
change at all.

The air is the gaps: between spans, below the lowest, and above the highest. Gap `k`
of a cell with `n` spans sits beneath span `k`; gap `n` runs from the highest span to
the grid's ceiling and is open sky. Every stored gap is at least `SPAN_SLAB_M`
(0.25 m) tall — `spanInsert` merges bodies closer than that rather than leaving an
air gap nothing fits through, which is what stops a wall standing on a floor from
reading as a hollow shell.

### Surveyed, and why an empty cell is not empty sky

A cell that no band sheet covered has an empty span list, and an empty span list has
exactly one air gap running 0..ceiling with nothing above it — which the
classification would read as uncovered, reachable and therefore **OUTDOORS**, with
`current()` true. That is the single most dangerous answer this system can give: it is
confident, it is wrong, and it suppresses every consumer's raycast fallback for
geometry nobody has ever looked at.

So `buildGrid` also returns a **survey mask**, one byte per cell, set where any sheet
covered it. `classify` labels every gap of an unsurveyed cell `AIR_UNKNOWN`.
`surfaceTop` and `coverageDetail` return false there; `buildSurfaceGrid` drops through
to its heightmap fallback; `traceSolid` returns false when the DDA crossed one
(`SSAcoustic::Trace::mUnsurveyed`); the acoustic bake refuses to anchor a probe on one
and its wall rays stop at the boundary rather than running to the 64 m reach cap over
geometry nobody has seen.

An unsurveyed cell also **blocks** the walk — walking through it would be walking
through geometry nobody has looked at. That has a consequence which is easy to get
wrong, and which the first attempt did get wrong: **what the blocking seals off is not
proven interior, only unproven.** A porch or an awning whose only route to open air
crosses the survey boundary is never reached by the flood, and calling that INTERIOR is
the original bug with its sign flipped — a confident wrong answer, taken by every
consumer, that reads as sealed-room ambience, `enclosureAt` of 1, and a killed sky
veil, on the fringe of every partially surveyed region.

So step 4 runs a **doubt flood**: every unreached gap in a column that borders an
unsurveyed one is UNKNOWN, and the doubt floods through the rest of that unreached
pocket. A pocket that touches no unsurveyed column is sealed by *real geometry* and
stays INTERIOR. `gap_label` doubles as the visited mark, so this costs no extra plane.
The rule in one line: **a pocket sealed by the survey boundary is UNKNOWN, not INTERIOR.**
Note the limit of that claim. The doubt flood covers air the walk never *reached*. Air it did
reach, and merely ran out of budget in, stays INTERIOR even in the last surveyed column - a long
low corridor running out to the boundary ends INTERIOR, and that is intended. The budget is a
statement about surveyed geometry along a surveyed path, so the only error mode there is an unseen
*second* opening; extending doubt to budget-exhausted boundary air would put every warehouse whose
wall happens to sit at a moving flycam's survey edge into UNKNOWN.

This is not a corner case. Three routes reach it in ordinary use:

- The census envelope (`SSWorldFieldShapesRange`) is an agent-centred square, while
  the field keeps grids for regions within 64 m of the **camera**. A flycam routinely
  leaves part of a region unsheeted.
- A band that built while `SSWorldField` was off holds no sheet, and `schedule()` only
  re-enqueues a band whose geometry signature moved — so it would hold none for ever.
  `SSNavMesh::resheet()`, called on the field's off→on edge, invalidates exactly those
  signatures.
- A band build that bailed out before filling its sheet leaves the vectors empty, and
  the adapter skips it.

`airCoverage(region)` is the surveyed fraction, counted once by the worker — which is
also what it takes to be a real build-progress signal rather than a constant 1.

**The dense grid is a materialised view, not a store.** It is built from the sheets
in the worker, published as a unit, never edited in place, and thrown away whole
when the next one replaces it. The sheets are the archive; the grid is the answer.
That is the same "capture sparse, solve dense, archive sparse" shape the 2026-09-01
Design D argued for — it just turns out the sparse archive was already being
produced by somebody else.

### The classification

`SSWorldFieldCore::classify` is three walks over the gap graph. Two gaps of
neighbouring cells are adjacent when their z-intervals **strictly** overlap; a
corner touch where one gap ends exactly where the neighbour's begins is a wall
junction, not a door.

**1. Reachability.** Every cell's top gap is open sky and every gap of a border cell
can walk out sideways. A flood from those marks the air connected to outside at all.
What the flood never touched is sealed, and is INTERIOR by construction — no budget,
no distance, no argument.

**2. The reach budget.** A gap with structure over it is COVERED. An *opening* is a
reachable uncovered gap touching covered air. It hands its covered neighbours a
budget in **metres**, and every cell step spends one cell width:

```
capacity(gap)  = gap_height * cot(theta)          theta = SSWorldFieldOpenAngle (45 deg)
budget(next)   = min(budget(cur), capacity(next) * SHELTER_MULT) - cell_m
```

`SHELTER_MULT` is **2** and is a constant rather than a second setting, because it is
the same geometry as `theta` and the two should not be tunable apart. It shipped at 4
for a day; the owner's verdict on 2026-09-11 was that 4 made a 20 m-ceilinged
warehouse read about 80 m sheltered, which is most of a region.

The budget is counted in **whole cell steps**, not in metres or decimetres. That is not
a detail: the first implementation stored it in decimetres, and `toDm(0.25)` is 3 — so
at the shipping 0.25 m cell the walk spent 0.3 m of budget per 0.25 m of travel and
recorded distances 20% long. The reach came out at 83% of what the geometry said, which
combined with `SHELTER_MULT` going 4 to 2 gave about 42% of the pre-round build where
the owner had asked for 50%. A step is now exactly one step, the arithmetic is
integral, and sub-cell precision would be false precision anyway — one cell is the
grid's whole resolution. The covered distance likewise carries the BFS *level* and is
turned into metres once, at the end, as `level * cell_m`; the residual error is then
the half-decimetre of the U16 storage and does not accumulate.

The `min()` is the whole point, and it is where this differs from every earlier
version. The budget is re-evaluated against the **local** gap height at every step,
so it is capped by the tightest place the path has been through. A gap that pinches
under a low beam cuts whatever the tall part was carrying; a gap that stays tall
keeps it.

**3. The covered distance**, as a SEPARATE shortest-path walk over the air step 2
reached. It must not be the widest path's own length: a cell one step inside a narrow
door that is also reachable from a wide door 30 m away would record 30 m, and that
figure feeds the enclosure ramp, `airDepthAt` and the acoustic bake's
travel-to-outdoors. Every step costs the same cell width, so plain BFS order is
already shortest and this costs one queue pass. Stored per gap in **decimetres**, in
the output array, which is also what retired the separate float plane the budget walk
used to keep.

**4. The labels.** Covered air within ONE unmultiplied `capacity` of its **nearest**
opening still has the sky overhead at `theta` or better, so it reads OUTDOORS. Past
that but still in budget, SHELTERED. Out of budget, or never reached, INTERIOR.

Widest-path first (a max-heap on remaining budget), because the strongest opening
must be the one that decides how deep the shelter reaches. First pop is final: every
later entry carries a smaller budget.

#### What this buys, in cases

| Scene | Gap height | Outdoors reach | Shelter reach | Reads |
|---|---|---|---|---|
| Ground under a 200 m sky platform | ~199 m | 199 m | 398 m | OUTDOORS across any plausible footprint |
| Mid-air under that platform's deck | ~199 m | 199 m | 398 m | OUTDOORS (the symmetric case) |
| Under a 3 m eave | 3 m | 3 m | 6 m | OUTDOORS at the lip, SHELTERED a few metres in |
| A 2.4 m room, open door | 2.4 m | 2.4 m | 4.8 m | OUTDOORS ~2 m in, SHELTERED to ~5 m, INTERIOR beyond |
| A 20 m warehouse, open door | 20 m | 20 m | 40 m | OUTDOORS 20 m in, SHELTERED to 40 m, INTERIOR beyond |
| Past a 0.6 m beam in a 20 m hall | 0.6 m at the beam | 0.6 m | 1.2 m | the beam cuts the hall's budget to its own |
| Sealed box | — | — | — | INTERIOR, never reached |

The first two rows are the owner's rule and the reason the fixed `sqrt(aperture)`
seed had to go. That seed measured the *pixel count* of an opening, so it moved
whenever `SSWorldFieldCell` did, and it had no way to express "there is 200 m of air
above this ground, the platform is irrelevant". `gap_height * cot(theta)` is a
length, it is scale-invariant, and it says the physically obvious thing: how far you
can walk under a roof before the sky stops being up there depends on how high the
roof is.

Every row above is a test in `V:\Scratch\atmo\tests\worldfieldcore.cpp`.

### The point query — decision 2

A 3D point usually lands in a gap of its own cell. A flying avatar, rain at 200 m,
and a listener above a sky platform all sit in their column's **top** gap, which the
grid deliberately extends past its own ceiling so those answers exist rather than
falling off the end of the data.

The one case with no air at the point's own height is a point **inside a body** — a
camera pushed into a wall, an ear inside a floor slab. `resolveGap` walks DOWN from
`z` to the first gap within `SSWorldFieldGroundReach` metres (default 64) and answers
with that gap's verdict. Nothing within reach is open air, which the callers read as
OUTDOORS.

The split that matters for consumers: **no grid for the region is UNKNOWN; no
structure within reach inside a grid is OUTDOORS.** The first keeps every "the caller
keeps its raycasts" fallback contract intact; the second is the decision-2 rule.

### The enclosure ramp

`enclosureAt` is `d / (d + tau)` on the covered distance `d`, where

```
tau = max(SS_WF_ENCLOSURE_TAU_M, local_gap_height * cot(theta))
```

The same geometry that decides how far an opening carries outdoors decides how fast
the ramp saturates. One setting governs both, and a tall space never reads as
enclosed as a low one at the same distance from its opening. The 4 m floor stops a
gap thinner than a person from saturating instantly.

### Where the classification runs, and why not in the band worker

The tempting answer is to classify inside `buildBand`, where the `rcHeightfield` is
already in hand and hot. It does not work, and the reason is worth writing down
because it will come up again:

**Span extraction is local; classification is not.** Whether a cell is OUTDOORS
depends on a reach budget propagated from openings that may be tens of metres away,
across several columns and across band boundaries. A 16 m column cannot answer it.
A band cannot answer it either: a room whose floor is in one band and whose ceiling
is in the next is one air interval, and only a pass that merges the column's bands
sees that.

So the split is: **extraction in the band worker** (already there, already free,
already producing exactly the right thing), **classification region-wide on a
separate worker**. Which is also what makes the sheets worth keeping — the second
pass needs them again, and re-running Recast to get them back would be absurd.

## Staleness, threading and lifetime

This is the part the two dependent workstreams have to trust, so it is stated as
invariants rather than as a narrative.

- **One stamp, two serials.** `SSNavMesh::collectSheets` returns a stamp mixing every
  live band's key and geometry signature. `navSettle` runs only for regions
  `SSNavMesh::regionSettled` reports quiet (nothing queued, nothing building, no
  schedule pending); when the stamp differs from `mWantStamp` it stores the new one
  and bumps `mGeomSerial`. `mGridSerial` is the serial the published grid carries.
  `mGridSerial != mGeomSerial` means exactly "a rebuild is owed".
- **An unchanged stamp is a no-op.** A settle that rebuilt nothing must not re-run a
  region-wide walk. The cell setting is mixed into the stamp, because a grid built at
  0.25 m does not describe the world at 1 m and nothing else would notice.
- **A debounce.** A region under edit republishes bands in a trickle and
  `regionSettled` goes true between them. `SETTLE_DEBOUNCE` (0.5 s since the stamp
  last moved) is what stops one edit from costing five region walks.
- **One job at a time.** `mBuildBusy` is a plain bool; a region that settles while a
  job runs simply gets its turn on a later `update()`.
- **Two gates on the result.** A worker result is stored only if the generation
  counter has not moved AND the geometry serial still matches. The generation exists
  because a region handle can be re-created after eviction with a grid restarted at
  serial 0, which the serial gate alone would not catch. `clear()` and `evict()` both
  move it.
- **Published grids are immutable.** Everything the worker produces is swapped in by
  `std::move` in one completion. Nothing mutates a live grid in place. A main-thread
  reader always sees either the previous grid entire or the new one entire — which is
  what lets `traceSolid`, `probesAt` and `propagationQuery` read with no lock.
  (The one exception is tier B, which fills per-probe bundle *fields* in later
  batches, each individually serial-gated; a reader mid-fill sees a probe with
  `mHaveBundle` 0 and the tier A statistics, which is a valid answer.)
- **Span reads are served from a stale grid; label reads are not.** `surfaceTop`,
  `coverageAt`, `coverageDetail` and `traceSolid` answer from whatever grid is
  published, because a slightly old but self-consistent geometry is a better answer
  than none. `airLabelAt`, `airDepthAt`, `enclosureAt`, `acousticAt`, `probesAt`,
  `propagationQuery` and `acousticDebug` all require `current()`.
- **The retrace gate is `mGridSerial`.** `SurfaceGrid::mGeomSerial` and `validTiles`
  hand out the serial of the spans the caller is about to read, not the one the
  navmesh has already moved.
- **Nothing reads a setting off the main thread.** Every setting a job needs is read
  in `scheduleGrid` and captured by value.

## Handoff — the API the wind flow and acoustics worktrees build against

> This section is the contract. If you are the wind flow map or the acoustics
> workstream, this is what you may rely on; everything above it is rationale.

### Ownership

| Thing | Owner | Lifetime |
|---|---|---|
| `SSNavMesh::SpanSheet` | `SSNavMesh::Band::mSheet` | the band's life; replaced whole on rebuild, never edited |
| the region cell grid + labels | `SSWorldField::Tile` (private) | until the next classification replaces it |
| the acoustic probe graph | `SSWorldField::Tile::mAcoustic` | same |

You do not get a raw pointer into a `Tile`. You get **`snapshotGrid`**, below, which
hands out `shared_ptr`s to the published arrays — and those are safe to hold and read
from a worker for as long as you keep them.

### Threading rules

1. **Every `SSWorldField` query is main-thread only**, including `snapshotGrid`
   itself. They resolve a region through `LLWorld`, which is not thread-safe.
2. **The data a snapshot points at is not.** A published grid is built whole by a
   worker and swapped in by one completion; the arrays are never reallocated,
   reordered or rewritten in place, and a reclassification builds a *new* set rather
   than editing the live one. So a `GridView` taken on the main thread is valid on a
   worker indefinitely, across a reclassification and across the region's eviction.
   This is the supported way to get world-field geometry off the main thread.
3. **The one exception to "never written in place"** is tier B's acoustic store-back,
   which fills per-probe statistic fields inside `mAcoustic.mProbes` from its own
   main-thread completion. `GridView` does not expose the probes, so it is not your
   problem; if you read probes, read them on the main thread and treat
   `mHaveBundle` as the "tier B has landed for this probe" flag.
4. **Snapshot, never reference.** Capture by value or by `shared_ptr`. Never capture a
   `Tile*`, a raw span pointer, or an `LLViewerRegion*`.
5. **Read settings on the main thread** and capture them.
6. **Gate your result on a generation and a serial**, the way `scheduleGrid` does.
   `GridView::mSerial` is the serial of the grid you snapshotted;
   `gridSerial(region)` and `gridStale(region)` are the main-thread forms.

### The bulk read

```cpp
struct GridView
{
    std::shared_ptr<const std::vector<F32> > mSpanBottom, mSpanTop;   // [k*res*res + col]
    std::shared_ptr<const std::vector<U8>  > mSpanFlags;              // SSRainShadowMap::SURF_*
    std::shared_ptr<const std::vector<U8>  > mGapLabel;               // [col*(maxSpans+1) + k]
    std::shared_ptr<const std::vector<U16> > mGapDepth;               // DECIMETRES covered
    std::shared_ptr<const std::vector<U8>  > mSurveyed;               // [col], 0 = never looked at
    LLVector3 mOriginAgent;   // the (0,0) cell corner in agent space
    S32 mRes; F32 mCell; F32 mCeiling; S32 mMaxSpans;
    U32 mSerial; bool mStale;
};
bool snapshotGrid(U64 region_handle, GridView& out) const;
```

`col = y * mRes + x`. Cell `(x, y)` covers agent XY
`[mOriginAgent + (x, y) * mCell, + mCell)`. Z is **absolute agent Z** — the navmesh's
frame re-bases XY only, never Z, so a span's `[bottom, top]` is directly comparable
with an avatar's `mV[VZ]`. Above `mCeiling` there is nothing; a cell's top gap is
reported as extending past it so a query aloft still resolves.

`SSWorldFieldCore::buildGrid` is public and pure if you want to materialise at your
**own** resolution instead — take `SSNavMesh::collectSheets` on the main thread, turn
the `BandSheet`s into `SheetRef`s, and call it on your worker. Do that rather than
writing a second sheet-to-grid mapping: the lattice phase below is easy to get half a
cell wrong and nothing would tell you.

### The coordinate frame, exactly

The navmesh's 16 m column lattice is pinned to `mOriginGlobal`, the agent's region
origin at init, and region origins are multiples of 256 — so a column corner always
lands on an exact multiple of 16 m in any region's local frame, and 16 divides evenly
by every `SSWorldFieldCell` value that matters. `buildGrid` still rounds with
`floorf(x / cell + 0.5f)`: that is a **guard against float drift in the division**, not
a half-cell offset. If you build your own grid, use the same expression. If your cell
size does not divide 16, your cells straddle sheet boundaries and you will union
across them — which is legal but lossy at exactly the openings the classification
cares about.

`mCeiling` is `max(band zMax over the region's sheets) + 8 m`, floored at 32 m. It is
where "open sky" starts, so a consumer that needs to agree with the field about what
is outdoors must use the field's value, not its own.

### Queries — geometry (served even when stale)

```cpp
bool surfaceTop(const LLVector3& pos_agent, F32& z, U8& flags) const;
```
The highest solid span top in the point's cell, absolute Z, with
`SSRainShadowMap::SURF_*` flags. False when no grid covers the point, when the cell is
**unsurveyed**, or when the column is fully sky. *Guaranteed:* if it returns true, `z`
is a real surface the census saw.

```cpp
bool coverageDetail(const LLVector3& pos, bool& covered, F32& ceiling_z, F32& column_top_z) const;
```
`ceiling_z` is **the underside of the lowest span above the point** — a real ceiling.
(Before this round it was that span's *top* face, because the depth-peel capture only
ever resolved top faces. It is now a slab thickness lower, and correct.)
`column_top_z` is the highest span top. False when no grid covers the point.

```cpp
bool traceSolid(const LLVector3& a, const LLVector3& b, F32& solid_m, S32& crossings) const;
```
2D DDA over the cells the segment crosses, z-interval intersected against each cell's
spans. Exact against the grid, no scene raycast, ~4 cells per metre at 0.25 m.
**Both endpoints must be in one region's grid** — a partial count would read as
confidently open air, and for audio that is the optimistic direction. False
otherwise; keep your heuristic for that case. *Best-effort:* accuracy is the
navmesh's — a phantom prim or a `DYNAMIC` mover is not in it.

```cpp
bool buildSurfaceGrid(U64 region_handle, S32 n, SSRainShadowMap::SurfaceGrid& out);
static bool buildDrainage(const SSRainShadowMap::SurfaceGrid& grid, Drainage& out);
```
Unchanged contracts. `buildDrainage` is `static` and pure — safe on a worker.

(`coverageAt`, `tileValid` and `geometrySerial` were deleted on 2026-09-11: no caller
outside the field, and `geometrySerial` returned the *grid* serial despite its name,
which is a trap. Use `gridSerial`.)

### Queries — classification (require `current()`)

```cpp
U8  airLabelAt(const LLVector3& pos) const;   // AIR_OUTDOORS / SHELTERED / INTERIOR / UNKNOWN
U32 airDepthAt(const LLVector3& pos) const;   // METRES of covered travel, or AIR_DEPTH_UNREACHED
F32 enclosureAt(const LLVector3& pos) const;  // 0 outdoors .. 1 interior, or -1 for no answer
F32 enclosureAtRegion(U64 region_handle, const LLVector3& pos) const;
F32 airCoverage(U64 region_handle) const;     // 1.0 once a grid is current
```

- `airLabelAt` **no longer returns `AIR_SOLID`.** A point inside a body walks down
  (decision 2). `AIR_SOLID` still exists in the enum and in the per-gap arrays, where
  it means "no such gap".
- `airDepthAt` is **metres**, not graph hops. The old figure counted cell steps and
  therefore changed meaning whenever `SSWorldFieldCell` did.
- `enclosureAt` returns **-1 for "no answer"** — no grid, or stale. Do not treat -1
  as outdoors. It returns **0** for "open air, nothing overhead within reach".
- `enclosureAtRegion` is the bulk form: resolve the region once, then loop. Use it
  when you are walking a window of cells.

### Queries — acoustics (require `current()`)

`acousticAt`, `probesAt`, `propagationQuery`, `acousticDebug` and the
`ProbeSample` / `Propagation` / `AcousticDebug` structs are **unchanged in shape and
meaning**. One unit changed underneath them: `SSAcoustic::Probe::mTravelM` is now
derived from the covered distance in decimetres rather than cell hops, so it is in
metres and no longer moves with the cell size. `Probe::mGapDepth` is decimetres.

### What is guaranteed vs best-effort

**Guaranteed**

- A published grid is internally consistent: spans, labels, depths, the survey mask
  and the probe graph all describe the same snapshot.
- The span list of a cell is **sorted and disjoint**, with every air gap at least
  `SPAN_SLAB_M` (0.25 m) tall, however the bands interleave and whatever order the
  sheets arrived in. (This was not true until 2026-09-11: a body merging into the span
  below could widen it clean past the span above. A pillar standing on a floor slab
  and passing through a ceiling slab reproduced it.)
- A cell the field has not surveyed answers UNKNOWN, never OUTDOORS - and never
  INTERIOR either, directly or by sealing a pocket off behind it. **An unreached pocket
  touching unsurveyed ground is UNKNOWN.** Air the walk reached and merely exhausted its
  budget in stays INTERIOR even against the boundary; that verdict rests on surveyed
  geometry along a surveyed path.
- The covered distance is exact to the storage granularity (half a decimetre) at every
  cell size, and does not accumulate per-step rounding. The budget is integral in cell
  steps.
- No acoustic probe is ever anchored on an unsurveyed column, and no wall ray runs
  through one.
- A query never returns data from a different serial than it reports.
- Cells are region-anchored and stable across region crossings (the navmesh's own
  frame is pinned at init; the field re-bases through region origins).
- `SS_WF_MAX_SPANS == SSNavMesh::SpanSheet::SPANS` and the core's air-label constants
  equal `EAirLabel`, both `static_assert`ed.
- Nothing in `ssworldfieldcore.h` reads a setting, the camera or a system. It is a
  pure function of its arguments and is pinned by the scratch harness.

**Best-effort**

- **The navmesh is the only geometry source, and its defaults were deliberately
  flipped.** `SSNavMesh` needs `SSWorldFieldShapes`; both shipped `0` and both now
  ship `1`. This is an **owner decision of 2026-09-11**, taken knowingly and weighed
  against the alternatives, not an incidental edit: `SSWorldField` itself defaults on,
  the depth-peel fallback that used to cover a fresh profile is gone, and a field with
  no geometry source is inert — so leaving those two off would have shipped a system
  that silently does nothing for everyone who has not gone looking in Debug Settings.
  The cost is the census scan and the Recast band builds running by default. With the
  navmesh not running the field answers nothing and every consumer sits on its
  fallback; `update()` warns once under `SSWorldField` when that is the case.
- **Coverage follows the census envelope.** `SSWorldFieldShapesRange` and
  `SSNavMeshRange` bound what the navmesh knows. Outside them there are no sheets —
  and since 2026-09-11 those cells answer **UNKNOWN**, not "outdoors". A sky platform
  200 m above a ground-level camera is usually not in the census, so the reach rule
  never gets to see it; the ground below reads outdoors because its own column is
  sky-open, which is the same answer for a different reason.
- **Latency.** A prim edit costs a census rebuild, band rebuilds, a settle, a 0.5 s
  debounce and a **150 ms** region classification. One to three seconds end to end,
  dominated by the census and Recast rather than by anything here.
- **`DYNAMIC` records are invisible.** Movers ride the tile cache as obstacles and
  never enter a layer or a sheet. A moving vehicle does not occlude, shelter or
  enclose.
- **Phantom and no-physics prims are invisible**, by `navIgnores`.
- **One region at a time.** `traceSolid` and the propagation graph stop at a region
  border.

### Query costs, so nobody puts one in a loop

| Query | Cost | Notes |
|---|---|---|
| `surfaceTop`, `coverageAt`-shaped reads, `airLabelAt`, `airDepthAt`, `enclosureAt` | O(spans), a handful of loads | region resolve dominates; use `enclosureAtRegion` in a loop |
| `traceSolid` | O(cells crossed), ~4 per metre at 0.25 m | a 100 m segment is ~400 cells x spans |
| `acousticAt` | nearest probe in a lattice cell + its 8 neighbours | cheap |
| `probesAt` | `listenerProbeSet` + up to 4 weights | cheap since the dedupe marks became a kept plane |
| `propagationQuery` | **an unbounded Dijkstra over the region's probe graph, on the main thread** | the soundscape calls it per event, not per frame. Do not call it per source per frame |
| `airCoverage` | O(1) | a counter the worker stored |
| `buildSurfaceGrid` | O(n²) over the requested grid | the caller caches on the serial |
| `snapshotGrid` | six `shared_ptr` copies | free; take it as often as you like |

### Build progress and staleness, for a consumer that has to wait

- `gridSerial(region)` — 0 means no grid at all. Any other value is the serial of the
  published one; it changes exactly when a new grid lands.
- `gridStale(region)` — a reclassification is owed. Span reads still answer (from a
  consistent, slightly old grid); label reads return UNKNOWN / -1.
- `airCoverage(region)` — the **surveyed fraction**, 0..1. This is the progress bar:
  it rises as the census envelope reaches more of the region. It is not 1 just because
  a grid exists.
- There is no "tier B finished" event. Read `SSAcoustic::Probe::mHaveBundle` per probe.

### Which parts of the acoustic bake are the acoustics workstream's

`ss_wf_acoustic_build` lives in `ssworldfield.cpp` and rides `scheduleGrid`'s job, its
snapshot and its gates. That placement is the field's, and it should stay: probe
placement needs the whole region's gap structure at once and the field is the only
thing that has it.

**Yours to redesign:** everything inside `ssacousticcore.h` — probe placement rules,
the link/aperture model, the statistic bake, the Dijkstra cost function, the tier B
bundle — and the shape of `Probe`, `Link`, `ProbeSample`, `Propagation`.
**The field's, do not change without saying so:** that the bake runs inside the
classification job from the same snapshot, the serial/generation gates, the two-tier
split with tier B fanned out behind it, and the `Snap` layout (it is the published
grid's own layout, which is why it costs nothing).

### If you need something that is not here

The three extension points that will not fight the design:

1. **A new per-cell channel** — add an array to `Tile`, fill it in the same worker
   job from the same snapshot, publish it in the same completion. It rides every
   gate for free.
2. **A new pure algorithm over the grid** — put it in `ssworldfieldcore.h` and pin it
   in `V:\Scratch\atmo\tests\`. Keep it a function of its arguments.
3. **Worker access to geometry** — `collectSheets`. Do not add a second path.

What *will* fight the design: any API that hands out a pointer into a `Tile`, any
mutation of a published grid, and any second source of span geometry.

## Channels

The channel/interest machinery survives unchanged in shape. What changed is that
grids are classified for every region in reach regardless of claims — the work is one
job per region per settle and gating it behind a claim bought nothing — so an
`Interest` now only turns on *optional tiers*. Today that is exactly one thing: a
claimed `ACOUSTIC` channel plus `SSWorldFieldAcousticsQuality >= 1` enables the tier B
stochastic reverb bake.

| Channel | Built from | Consumers |
|---|---|---|
| `SURFACE_TOP` | the highest span per cell | precipitation landing, wet/snow/puddle field, runoff |
| `SOLID_VOLUME_3D` | the cell grid's spans | `traceSolid`, future wind occlusion |
| `COVERAGE` | the span stack + the classification | indoor/outdoor, burial, shelter; audio, fog, effects |
| `DRAINAGE_NETWORK` | priority-flood + D8 over a `SurfaceGrid` | runoff, puddles, streams |
| `WALKABLE` | — | the navmesh itself; this channel is now upstream, not downstream |
| `ACOUSTIC` | the gap-anchored probe bake over the grid | soundscape beds, reverb, occlusion, thunder |

`WALKABLE` inverting is the structural change worth noting: in the 2026-09-01 design
the navmesh was a *view over* the world field. It is now the world field's *source*.

## Costs

Measured, not estimated: `V:\Scratch\atmo\tests\worldfieldcore_cost.cpp` builds a
whole 256 m region at the default 0.25 m cell (1024 x 1024 cells, 7.34 M gap nodes) —
terrain everywhere, a 32 m roofed building every 32 m, and a 100 m sky platform — and
measures the classification's peak allocation through a global allocator hook.

| Item | Measured | Notes |
|---|---|---|
| Band sheets on the navmesh | ~50 KB per band | 64² cells x 6 spans x 9 B; only bands the census reached exist |
| Published span arrays | **54.0 MB** | 6 slots x 1 M cells x (4 + 4 + 1) B |
| Published labels and depths | **21.0 MB** | 7 gaps x 1 M cells x (1 + 2) B |
| Published survey mask | 1.0 MB | one byte per cell |
| `classify` peak allocation | **45.7 MB** | of which 21.0 MB is the output it returns |
| `classify` scratch above output | **24.7 MB** | was 130 MB before the 2026-09-11 rework |
| `classify` wall time | **150 ms** | one core, one region, once per settle |
| Peak during a rebuild | grid x 2 + scratch | the old grid stays live until the completion swaps |
| Grids held | up to 4 (`MAX_TILES`) | LRU, with near-region displacement |
| Acoustic probes | ~10 KB | 8 m lattice |

The scratch was the part the first round got wrong: the classification kept two F32
planes of gap bounds (58.7 MB), a float budget plane and a float travel plane
(58.7 MB), and byte planes for reached/covered (14.7 MB). Now the gap bounds are
recomputed from the spans (two array reads, and they were always going to be in cache
behind the span walk), the travel plane folded into the `gap_depth` output the walk
produces anyway, the budget plane is U16 decimetres, and reached/covered are bitsets.
The wall time went **down** as well as the memory, from 183 ms to 150 ms, because the
recomputation is cheaper than the cache traffic the two extra planes cost.

Steady state is therefore about **300 MB** of published grids for four regions plus a
transient **~125 MB** while one of them reclassifies. The dial is `SSWorldFieldCell`;
every step up quarters the grid.

### Latency

A prim edit costs: a census rebuild, the navmesh's band rebuilds, `regionSettled`, the
field's 0.5 s debounce, and one **150 ms** region classification on a worker. Call it
one to three seconds end to end, dominated by the census and the Recast builds rather
than by anything here.

The owner has accepted that as shipping behaviour, and column-scoped re-seeding is
explicitly **not** to be built. It is worth recording why it would not have helped: the
reach budget propagates from openings that may be tens of metres away and across band
boundaries, so a rectangle around an edit is not a closed problem — removing one wall
panel can change the label of every cell in a building. A dirty-rect reclassification
could not have produced correct labels under this rule. That is a design consequence
of the outdoors rule, not a regression against the depth-peel capture's re-peel path,
which only ever patched *geometry* and always re-ran the flood whole anyway.

## Open questions

- **Does 0.25 m survive a dense SLMC-scale region?** ~76 MB per region x 4 published
  is real money, even with the scratch down to 24.7 MB. The owner's verdict on
  2026-09-11 was to keep 0.25 m and treat the scratch as the bug, which is done; the
  remaining question is whether `SSWorldFieldCell` at 0.5 m costs enough false
  shelter on real builds to matter. Measure, do not guess.
- **Six spans per cell.** Unchanged from the old store and still unmeasured against
  stacked flats. `extractSpanSheet` already folds thinnest-gap-first at the sheet, so
  the overflow policy is applied twice; whether that compounds badly is unknown.
- **`SHELTER_MULT` = 2.** Set by the owner against the warehouse case, not measured
  across content. If a second dial is ever wanted this is where it goes — but it is
  deliberately not one today, because it is the same geometry as the angle.
- **OUTDOORS a gap-height inside a doorway.** At a 2.4 m ceiling the rule calls the
  first 2.4 m past the door outdoors. That is what a 45-degree sky cone genuinely
  says, and it is why `SSWorldFieldOpenAngle` is a setting — but it may want to be
  tighter for rain specifically, which lands vertically.
- **Cross-region `traceSolid`.** Still one region. Stitching wants the grids to agree
  at the border, which they do (the navmesh's frame is shared) — it is a walk across
  two grids, not a data problem.
- **The census envelope vs the classification.** The reach rule can only see
  structure the census has. Widening `SSWorldFieldShapesRange` for the field's sake
  costs the census; the two budgets are currently unrelated and probably should not
  be.
- **Design F and Design H** (below) are unresolved as before. Design H's premise
  — that `refineEdge`'s raster walk is the accuracy ceiling for eave lips — is
  unchanged by this round; the lips now come from the navmesh's 0.125 m raster rather
  than an ortho capture, which is a different, finer ceiling, but still a ceiling.

## Research notes

- **Recast / Detour TileCache** — navmesh from rasterised spans is the industry
  pipeline (voxelise → filter → regions → contours → polymesh). The heightfield *is*
  the span store, which is why this round is a deletion rather than a port.
  (recastnavigation, Docs/1_How it works; DetourTileCache.)
- **Project Acoustics** (Microsoft) — voxelise, bake per-probe acoustic parameters,
  interpolate at runtime. The ACOUSTIC channel is the statistic-based, wave-solver-free
  version of the same shape.
- **Steam Audio baking** — baked static-geometry propagation with a dynamic listener;
  the same probe/parameter split.
- **Portal/graph propagation** (Thief: Deadly Shadows and successors) — propagation
  over the navigation structure, which is now literally what this is.
- **Sky view factor** (urban climatology) — the fraction of the hemisphere visible
  from a point, and the standard measure of how "outdoor" a street canyon is. The
  `gap_height * cot(theta)` reach is the cheap graph-walk analogue: rather than
  integrating the hemisphere per cell, it propagates the one elevation angle that
  matters from the openings inward.
- **Priority-flood depression filling + D8/D∞** (Barnes et al. 2014; Tarboton 1997) —
  the hydrology-standard drainage computation, unchanged.
- **Mean free path / Sabine estimation** — the cheap reverb characterisation behind
  the tier A bake.
- **Layered depth images / depth peeling** — the capture primitive the deleted path
  used. Recorded because it was the right tool when nothing else rasterised the scene,
  and it stopped being the right tool the moment something did.

---

# Archive

Everything below is the 2026-09-01 design, kept verbatim. It describes a capture
service that no longer exists. It is kept because the adversarial reviews are the
record of why the current shape is the current shape, because Design F (physics-sweep
capture, rejected) and Design H (analytic edge refinement, unbuilt) are still live
decisions, and because the brief for that round is still the brief.


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

#### Where the code stands today

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

#### What the store must answer

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

### Part 1 — the substrate: four designs

#### Design A — column span store (the archived proposal, updated)

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

#### Design B — sparse voxel hash grid

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

#### Design C — one dense tile, everything from it

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

#### Design D — spans as the archive, dense as the answer (synthesis)

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

### Part 2 — the capture service

#### Design E — one peel capture (recommended core)

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

#### Design F — physics-sweep capture (rejected, recorded)

Build the store from the physics representation (convex decomposition AABB
sweeps) instead of renders. Rejected for the reason the archived doc gives and
the current code confirms: the capture must see what a drop, an ear and the
wind see — including non-physical megaprims (1024 m sim-surrounds), alpha with
depth writes, and terrain — which is a *render* property. Physics AABBs stay
where they already are: in the dirty-detection path (`markDirty`'s
AABB testing), not in the capture.

#### Design G — two-tier capture (recommended complement)

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

#### Design H — worker-pool analytic refinement over copied geometry (recommended precision layer)

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

### Part 3 — channels

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

#### SURFACE_TOP

Today's `SurfaceGrid` verbatim — one span's own fields — so `SSSurfaceField`,
the runoff shed, the wet pass, the granular transport and both GPU windows are
untouched consumers with unchanged contracts. The only change is upstream:
the grid is built from the shared store's top spans instead of a private
capture, and `refineEdge`/`resolveColumn` become reads of the same store at
full capture resolution.

#### SOLID_VOLUME_3D and the sparse air solve

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
>
> **Addendum (2026-09-07) - resolved, differently.** The gate above is
> satisfied: the capture grew a second shot per band (an upward pass whose
> depth comparison is reversed, so it returns the topmost body's underside
> without any peeling), and the store became true column span lists -
> [bottom, top] solid pairs per column, band planes healed at conversion.
> Rooms, underpasses, jetties and stilt spaces now exist in the store as one
> air interval each, and the flood's labels (outdoors / sheltered / interior)
> are computed over those air gaps directly. The wind solve's interior-skip
> is no longer gated on capture shape: what remains is wiring the solve's
> mask to the gap labels. The note above is kept as the record of why the
> one-surface-per-band store was not good enough.

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

#### COVERAGE

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

#### DRAINAGE_NETWORK

The current shed is per-edge-cell with a reservoir, fed by a capture slope and
a delivery correction. The shared store makes it a proper hydrology pass at
negligible extra cost, on every span level that has a surface:

> **Implementation note (2026-09-07) - the landing-surface level ships.**
> `buildDrainage` (still over the landing surface; per-span waits on the
> multi-peel store) now runs the whole numbered list below at whatever
> resolution the caller's grid is: the flood and pool mask exactly as
> described, D8 with the eave rule as a *descent exclusion and a terminator*
> (a cell with an eave side is where the surface ends for the water on it -
> it keeps its catchment rather than passing it along the lip line, which is
> also what keeps a run's summed catchment from double-counting a sloped
> gutter), and flow accumulation in descending spill order into
> `Drainage::mCatch`. `SSSurfaceField` consumes it as the design intended:
> the network is materialised per region at `SSAtmoRunoffRes` (1 m cells,
> finer than the wet field's own 2 m lattice, which nothing of the wet field's
> changes), the lip cells flood-fill into runs with refined lip points walked
> against the 0.25 m store, and the liquid shed works per run - one
> reservoir per run filled by its catchment, curtain streams cut into the
> preset's span slots across it, and drips placed catchment-weighted so they
> gather where the roof's flows converge. Reservoirs survive a retrace keyed
> by each run's biggest feeder, with the water of runs that died shared out
> by catchment. The per-lip cursor shed remains as the granular transport's
> drain (creep debits the per-cell stores; the cursor drains them into
> cascades), because granular delivers to cells, not to catchments.

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
   archived runoff doc's shape) come back as the grouping layer for streams —
   which the 2026-09-07 note above shows landing for the landing-surface
   level: the run carries the summed catchment, the mean shed direction and
   the refined lip points, the stream slots and the drip placement both hang
   off it, and the field's drain-tau reservoir math is unchanged either way.
4. **The wet/puddle/snow field keeps its contract**: it still reads a
   `SurfaceGrid`-shaped geometry, still integrates per cell, still feeds the
   shed and the granular transport. What changes is who owns the surface it
   integrates on (the shared store) and that pools now come from real
   depressions instead of a noise mask over a slope test.
5. Bridges and underpasses are free: each span level is its own catchment.
   Water on the skyway drains the skyway; water under it drains the street;
   today's single-top-surface trace cannot even represent that.

#### WALKABLE (navmesh)

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

#### ACOUSTIC

> Superseded in detail by `doc/atmo_magic_acoustics.md` (2026-09-07): the
> gap-anchored probe set, the probe graph, the realtime occlusion trace and
> the two-tier reverb bake. The sketch below stays as the channel's original
> outline.

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

### Part 4 — partial updates and patching

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

### Part 5 — migration, in an order that never breaks the intermediate state

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
   the puddle mask's slope test becomes filled-depression membership. The
   landing-surface half of this step is built (2026-09-07): `buildDrainage`
   carries the fill, the pool mask, the eave-ruled D8 and the catchment
   accumulation, and the surface field's liquid shed reads runs off it —
   per-span levels still wait on the multi-peel store.
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
