# Atmo Magic: a shared world field (proposal)

> **OUT OF DATE** — archived 2026-08-26. Unbuilt proposal; WorldField is on the current backlog, so treat this as a starting sketch, not a spec.

This is a design, not a build log — nothing here exists yet. It sketches a
single geometry capture shared by everything that currently captures its own:
rain shadow, drainage, wind flow, and weather audio, with pathfinding,
parkour and combat as consumers the same store should be able to serve
without a fifth capture pipeline being written for each of them.

## The problem

Four systems currently ask the scene the same question — "what's actually
here, and what can see what" — and each asks it its own way:

| System | Capture | Shape | Anchoring |
|---|---|---|---|
| `SSRainShadowMap` | one ortho depth pass along the fall direction | 2.5D: one height + flags per cell | region tile, camera-relative band |
| `SSRunoff` | none — traces `SSRainShadowMap`'s grid | derived: flow direction, catchment, eaves | region-local, keyed to `SSRainShadowMap`'s geometry serial |
| `SSSurfaceField` | none — integrates `SSRunoff`'s network | derived: wet/snow/puddle per cell, over time | camera-centred GPU window, stitched from region fields |
| `SSWindFlowMap` | overhead ortho + bottom-up ortho + 4 oblique probes | 3D: solid mask per slab, carved for overhangs | region tile + margin, static band from the sky track |
| `SSWeatherSounds` | live raycasts every probe cycle: 3 up, 4 out | point samples, not cached | camera-relative, no persistence |

Three of those five run an actual GPU or raycast capture against the live
scene; two derive from one of the others. `SSWeatherSounds` already reaches
across and reads `SSWindFlowMap`'s captured column top for burial depth — the
one place this already happens — which is the proof that sharing works and
the reason it hasn't happened anywhere else: there was no shared thing to
reach into, only `SSWindFlowMap`'s own tile, borrowed because it happened to
be there.

`SSWindFlowMap`'s probe carving is also, undocumented as such, the only
existing *indoor/outdoor* classifier in the codebase — a cell is open if a
probe saw past it, solid otherwise. `SSWeatherSounds` reimplements the same
question with its own raycasts because `SSWindFlowMap`'s answer is a solid
mask sized for a pressure solve, not a queryable "is this point inside", and
there is no third shape between them that is both.

None of this is redundant *by mistake* — each was built exactly wide enough
for what it needed, in order, with the ground already covered by whatever
came before it repeated rather than reused because reusing it meant
generalizing something that was never designed to be. Fixing that on
purpose, once, is what this proposes.

## What it needs to do

Taken directly from the brief:

- **Update partially.** A prim edit should cost a tile, or a fraction of one,
  never a whole-world rebuild.
- **Anchor to the region and shift with it**, the way every tile above
  already does — this is not new ground, it is the one pattern all four
  captures already independently converged on.
- **Track enabled/disabled per consumer.** A build with drainage on and wind
  off should not pay for what wind needs, and vice versa.
- **Answer indoor vs. outdoor**, and **how far underground/buried**, at a
  point.
- **Distinguish a skyway from a solid block** — the exact problem
  `SSWindFlowMap`'s probes already solve, generalized so drainage and
  acoustics get the answer too instead of each growing their own probes.
- Be a foundation for **pathfinding, parkour, and combat** later, without
  those needing their own capture.

## Representation: column spans, not a voxel octree

The brief's instinct toward a sparse voxel tree is the right instinct aimed
at the wrong structure for what actually produces this data, and it's worth
saying why rather than just picking something else.

**Everything upstream of this is a depth capture.** `SSRainShadowMap` and
`SSWindFlowMap` both get their geometry from ortho depth-peel renders, and a
depth-peel render doesn't hand back voxel occupancy — it hands back, per
pixel, the distance to the *next* surface. Peel again excluding what the
first pass already found and it hands back the surface after that. What
falls out of that process, for free, is a **span list per column**: enter
height, exit height, repeat — not a tree of cubes. Turning peeled depth into
a voxel octree would be extra work to throw away the one thing the capture
method gives away for nothing, and would then have to be flattened right
back into spans (or a dense grid) for anything that reads it, because
nothing downstream wants to walk a tree per query.

**Everything downstream of this wants a dense grid, briefly.** The wind
solve is a Jacobi pressure relaxation over a regular 3D grid — that's not
incidental, it's what makes the stencil sampling and the boundary handling
tractable at all. A sparse structure buys memory efficiency exactly where
this domain doesn't need it: SL content is overwhelmingly stacked flats —
floors, roofs, streets, the odd skyway — which a column of 2-6 spans already
represents with almost no waste. True arbitrary 3D sparsity (a cave system,
volumetric fog) is the case a voxel octree earns its complexity for, and it
is not the shape of the content this is built from. `SSWindFlowMap` already
proves the pattern that works here: capture sparse-ish (spans), solve dense
(a tile-sized 3D texture materialized on demand), throw the dense copy away
when the tile goes stale. That's the shape to generalize, not replace.

So: **a region-anchored 2D grid of columns, each column a short list of
vertical spans.**

```cpp
struct WorldSpan
{
    F32 mBottom, mTop;       // agent-space Z
    U8  mFlags;              // solid / water / terrain / fallback - SSRainShadowMap::SURF_* extended
    U8  mOpenAbove : 1;      // nothing solid between mTop and the region's own capture ceiling
    U8  mOpenBelow : 1;      // nothing solid between mBottom and the ground/water beneath it
};

struct WorldColumn
{
    // Almost always 1-3; a handful of spans covers a multi-storey building
    // with a basement and a skyway over it without needing to say so up
    // front. Storage is a small inline array over a heap list, the same
    // trade every other short-vector case in this codebase already makes.
    llfixedarray<WorldSpan, 4> mSpans;
};
```

A column with one span is `SSRainShadowMap`'s current grid exactly — this is
a strict superset, not a rewrite of what it already stores. `mOpenAbove` on
the top span is "is this point outdoors"; a lower span with `mOpenAbove` true
*and* `mOpenBelow` true is a bridge, skyway, or platform — solid, and with
open air both over and under it, indistinguishable in the data from a floor
slab because that is what it is. No separate "is this a skyway" flag is
needed; it falls out of having more than one span.

## Capture: peel, then probe

**Base layer — vertical depth peel.** One ortho pass per span depth, same
machinery `SSRainShadowMap` and `SSWindFlowMap`'s overhead pass already use,
peeling deeper each time by excluding what the previous pass already
resolved. `SSAtmoWorldFieldMaxSpans` bounds how many peels a column ever
gets (2-4 is very likely enough — even a dense multi-storey build rarely
wants more spans than storeys plus a basement plus a roof deck). This
directly retires `SSWindFlowMap`'s documented **"two-layer capture"**
limitation rather than working around it again.

**Refinement — oblique probes**, exactly as `SSWindFlowMap` already runs
them: four cardinal rays, tilted down, evidence-only and OR'd, carving
solid back to open wherever a probe sees past it. A vertical peel cannot see
a vertical surface at all, which is the one thing it will never fix no
matter how many layers deep it goes — an arcade's inner wall, the underside
soffit of a deep overhang, both need a ray that isn't going straight down to
find. The probes stay; what changes is that they carve the **shared column
store**, once, rather than a wind-private solid mask that acoustics and
drainage can't see.

**Coverage answers "indoor vs. outdoor" directly.** A point is outdoor if
it's on or above a span with `mOpenAbove`. It's indoor/sheltered if it's
below one without. **Burial depth** is the gap between the point and the
underside of the lowest span *transitively* open above it — walking up the
span stack past however many floors intervene, rather than
`SSWeatherSounds`'s current single subtraction against `SSWindFlowMap`'s
overhead-only top, which cannot currently tell one storey down from three.

## Tiling and partial update

Unchanged from what already works, because what already works is right:

- **One tile per region plus margin**, region-local horizontal storage,
  absolute Z — `SSWindFlowMap`'s exact scheme, proven across a region
  crossing and a var-region alike.
- **Column-granular dirtying.** `markDirty(pos, radius)` already exists
  three times over (`SSRainShadowMap`, `SSWindFlowMap`, and implicitly
  `SSRunoff` through the geometry serial it keys off). One version of it
  here, marking the columns an edit's AABB actually covers rather than the
  whole tile, so a re-peel can be **scissored to the dirty rect** instead of
  redoing the region. A tile still carries a geometry serial the way
  `SSRainShadowMap`'s does, for the same reason: a recapture chasing the
  camera or the wind is not a reason to retrace drainage or resolve wind
  again.
- **Staged across frames**, the way `SSWindFlowMap::advanceBuild` already
  spreads a solve over several calls rather than stalling one frame for all
  of it. A multi-span peel is more GPU work per tile than today's single
  pass, so this matters more here, not less.

## Channels, not one big answer

Not every consumer wants the same thing out of the store, and materializing
everything for everyone is exactly the cost the brief's "track volume
enabled/disabled per subsystem" is asking to avoid. The column store itself
— spans, flags, coverage — is cheap and always on when Atmo Magic is;
everything built *from* it is a separate, independently toggled channel,
materialized lazily and only for tiles something has asked for:

| Channel | Consumer | What it is |
|---|---|---|
| `SURFACE_TOP` | precipitation, drainage | the old `SSRainShadowMap::SurfaceGrid`, now the top span's own fields |
| `SOLID_VOLUME_3D` | wind | dense per-slab mask, exactly `SSWindFlowMap`'s `mSolid` today |
| `COVERAGE` | audio, future pathfinding | outdoor / sheltered / buried-depth per point |
| `DRAINAGE_NETWORK` | runoff, surface field | unchanged: still derived from `SURFACE_TOP`, just sourced from the shared grid |
| `WALKABLE` *(future)* | pathfinding | span tops within a slope/step tolerance |

A consumer registers interest in a channel for the tiles it cares about; the
peel resolution, margin, and span depth actually captured is the union of
what every currently-interested channel needs, so a region with only
drainage enabled still runs the cheap single-layer peel `SSRainShadowMap`
runs today, and only pays for multi-span + probes when something — wind,
audio, later pathfinding — has actually asked for `SOLID_VOLUME_3D` or
`COVERAGE` there.

## Sketch of the API

```cpp
class SSWorldField : public LLSingleton<SSWorldField>
{
public:
    enum EChannel { SURFACE_TOP, SOLID_VOLUME_3D, COVERAGE, DRAINAGE_NETWORK, WALKABLE };

    // A consumer's stake in a channel over a region. Ref-counted per
    // (region, channel) pair; the field only ever builds what has a
    // holder. Destroying the last handle for a channel over a region lets
    // that region stop paying for it on the next tick, the same way a
    // channel nobody asked for was never built at all.
    class Interest;
    Interest claim(U64 region_handle, EChannel channel);

    const WorldColumn* columnAt(const LLVector3& pos_agent) const;
    bool coverageAt(const LLVector3& pos_agent, bool& outdoor, F32& buried_depth) const;

    // Existing per-system shapes, now views over the shared store rather
    // than owners of their own capture - signatures unchanged so callers
    // migrate for free.
    bool buildSurfaceGrid(U64 region_handle, S32 n, SSRainShadowMap::SurfaceGrid& out);
    bool solidVolume(U64 region_handle, /* ... */);
};
```

`Interest` is deliberately a handle, not a bool flag — a floater or a
prototype system that wants `COVERAGE` for a minute of testing shouldn't
need a separate "did I turn this off again" bookkeeping pass; it drops the
handle and the field notices.

## Migration, in an order that never breaks the intermediate state

Each step should leave the viewer working exactly as it does today, with
one fewer capture pipeline underneath it.

1. **`SURFACE_TOP` absorbs `SSRainShadowMap`'s single-layer capture.**
   `SurfaceGrid`, `resolveColumn`, `refineEdge` all keep their signatures and
   become thin reads over the shared column store's top span. Lowest risk:
   one span per column is exactly what exists today, so this is a plumbing
   change with no behaviour change to prove against.
2. **Multi-span peel lands**, gated off by default. Nothing consumes the
   extra spans yet; this step is purely "does the peel-and-exclude capture
   work and cost what it should."
3. **`SOLID_VOLUME_3D` absorbs `SSWindFlowMap`'s own overhead + bottom-up +
   probe capture.** This is the step that actually deletes a capture
   pipeline: wind's Stage `CAPTURE_TOP`/`CAPTURE_PROBE` become "ask the
   field for `SOLID_VOLUME_3D` over my tile" instead of running their own
   renders. The pressure solve itself is untouched.
4. **`COVERAGE` absorbs `SSWeatherSounds`'s live raycasts.** The up-probe and
   the four side-probes are replaced by a `coverageAt()` query. This is
   where the system starts paying for itself in the feature it improves
   rather than only in the pipeline it removes: burial depth becomes
   floor-count-aware for free, not just a nicer number.
5. **`SSRunoff` and `SSSurfaceField`** already only ever consumed
   `SSRainShadowMap`'s grid — after step 1 they're reading the shared store
   with no change of their own required.
6. New consumers — pathfinding, parkour, combat — start from `WALKABLE` and
   `COVERAGE`, which by this point already exist for other reasons.

Every step is independently revertable, and steps 3-4 are exactly where the
existing "known limitations" sections of the wind flow and audio docs stop
being limitations of a private capture and start being tuning dials on a
shared one.

## Where each mechanic makes another one better

This is the part actually worth building this for. The wind flowmap
advecting precipitation is the existing proof of the shape: one capture,
solved once, made *two* systems better than either would have been solving
it alone. A shared field is what makes that the common case instead of the
one deliberate crossover in the codebase.

- **Drainage → movement.** `SSSurfaceField` already tracks wet/puddle/snow
  depth per cell and currently spends it on shading alone. The same field is
  a path cost: wet ground already slows a real person down and changes how
  their footsteps sound, and both are a lookup into data that already
  exists, not a new simulation.
- **Coverage → tactical audio.** `SSWeatherSounds`'s burial depth already
  attenuates the rain bed by how much build stands overhead; the identical
  number answers whether a footstep two floors up should be heard at all,
  for **SLMC combat** specifically — occlusion by actual intervening structure
  rather than distance falloff pretending a floor isn't there.
- **Coverage → cheap occlusion / cover.** A column's span directly over a
  point at crouch or prone height *is* a cover query — solid between shooter
  and target's specific body height, answered from the same store, no
  physics raycast per shot. This is the "cheap occlusion queries" the brief
  asks for, and it is the same lookup `coverageAt()` already does for indoor
  classification, just asked at a different height.
- **Wind exposure → combat and effects.** The solved exposure channel
  already distinguishes a sheltered courtyard from a wind-scoured rooftop;
  smoke, gas, or a thrown grenade's drift are the same advection
  precipitation already rides, and a sniper's sightline down a wind-scoured
  lane is a reason to actually feel that lane differently than the courtyard
  next to it.
- **Spans → parkour.** A vaultable ledge or a mantle point is precisely a
  span boundary within jump height of a player's feet — the query parkour
  detection needs is a short walk up the same column stack wind and
  drainage already read, not a separate scan.
- **Multi-span + probes → pathfinding under structure.** The passages
  `SSWindFlowMap`'s probes already discover — the space under a skyway, an
  arcade, a bridge deck — are exactly the regions a navmesh generator has no
  cheap way to find today short of a full physics sweep. They come free with
  a channel wind already pays for.
- **Snow depth → footprints and sound**, once something reads it: currently
  tracked and unused. A muffled footstep and a compressed-snow decal are
  both a threshold read on a field already being integrated every tick.
- **Combat regions specifically.** An SLMC-style build lives or dies on
  whether cover, sightlines, and audible occlusion actually track the build
  rather than a flat "line of sight yes/no." Every one of the four points
  above is a piece of that read from one store instead of four bespoke
  systems each built for one game mode.

None of this is "and also build pathfinding" — it's that the shared store
turns pathfinding, parkour, and combat cover from *new capture pipelines*
into *new questions asked of an existing one*, which is the actual argument
for doing this at all rather than leaving five independent captures to grow
a sixth next time something new needs to know what's there.

## Open questions

- **Span budget under real content.** A genuinely deep, many-storey
  build with irregular floors could still blow past 4 spans in a column;
  needs measuring against real SLMC-scale builds, not assumed.
- **Peel cost at multi-span depth.** Each extra span is a full ortho pass;
  `SSAtmoWorldFieldMaxSpans` is the dial, and the right default is an
  empirical question once this exists, not a guess now.
- **Channel materialization granularity.** Per-region is `SSWindFlowMap`'s
  and `SSRainShadowMap`'s existing grain; whether `COVERAGE` specifically
  wants finer (per-column) staleness tracking so one edited column doesn't
  invalidate a whole tile's worth of audio classification is worth deciding
  once step 4 is actually being built, against real edit-churn behaviour.
- **Ownership of the probe carve.** Right now it is wind-solve-shaped
  (per-slab). Shared, it needs to answer "open or not" at an arbitrary
  height, not just at slab boundaries — a small interpolation question, not
  an architectural one, but it has to be answered before step 3.
