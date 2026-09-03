# Atmo Magic: the SSWater plane family

Atmo Magic renders its own water plane geometry - `SSWater`, `SSEdgeWater` and `SSFarWater` in
`indra/newview/sswater.h/cpp` - instead of bending the stock `LLVOWater`/`LLVOVoidWater` objects to
its will. The stock planes stay pristine: whatever we later do to Atmo water (tides from the
keyframed height, per-tile waterlines, horizon-reaching geometry) lands in the SS classes and the
stock viewer water is untouched and instantly recoverable by switching Atmo off.

## The family

| Class | Base | Covers | Status |
|---|---|---|---|
| `SSWater` | `LLVOWater` | one plane per connected region, at the environment's water height | live |
| `SSEdgeWater` | `LLVOVoidWater` | 256x256m void tiles ringing the regions out to the squash cap | live |
| `SSFarWater` | `LLVOVoidWater` | the band past the projection far plane | stub |

All three reuse the stock water machinery wholesale - `LLVOWater::updateGeometry`, the existing
`PARTITION_WATER`/`PARTITION_VOIDWATER` spatial partitions, `POOL_WATER`, the water shaders, water
haze, refraction and the exclusion mask - **and the stock pcodes** (`LL_VO_WATER` /
`LL_VO_VOID_WATER`), so every pcode-keyed check in the viewer (the static-drawable assert, octree
debug colours, the non-interactive guard, blocked-neighbour culling) treats both families
identically with nothing registered anywhere. The only discriminator is the `mIsAtmoWater` flag on
`LLVOWater`, set by the SS constructors. Because the pcodes are shared, the pcode factory cannot
build the subclasses: `SSWaterWorld` news them directly and hands them to
`gObjectList.adoptViewerObject`, a small helper that does the same bookkeeping
`createObjectViewer` would have.

## The swap

`SSWaterWorld` (singleton, `sswater.cpp`) ticks once per frame from `llviewerdisplay.cpp`, right
after `SSAtmoEnvApplier::apply()` so it reads the same frame's active state. When the applier is
active (Atmo enabled, asset loaded, tracks present) it builds the SS family and the stock planes
stop drawing; when inactive the SS family is destroyed and stock water returns. Exactly one family
renders on any given frame.

The suppression is at the two draw call sites, not at object lifecycle: stock water objects keep
existing (the region water object doubles as the region's water height store - `LLSurface` reads
its position - so it must never be killed). `LLDrawPoolWater::pushWaterPlanes` and the
`pushFaceGeometry` override (the water haze re-push) both gate each face through
`SSWaterWorld::drawsThisFrame`, which compares the face's `mIsAtmoWater` flag against the frame's
owner.
Both families share one `mDrawFace` list in the single water pool, so these two sites are the
complete set of render entry points.

## What drives the water look

Nothing new. The applier already installs its own `LLSettingsWater` into `ENV_LOCAL` and writes
every keyframed `SSAtmoEnvWater` param (fog colour/density, fresnel, normal map, wave speeds,
refraction scales, blur) into it per frame; `LLDrawPoolWater` reads `getCurrentWater()`. Since
SSWater renders through that same pool, the whole keyframed param set applies to SSWater and
SSEdgeWater together - one track, one look, sim water and void water in lockstep.

The **water plane checkbox** (`water_enabled_check`, `SSAtmoEnvWater::mEnabled`) also needs nothing
new: the applier's `setWaterRendering(false)` flips `RENDER_TYPE_WATER` off, which hides water and
void water alike - an empty void, water removed from the sim and the surrounding void together.
When neighbouring sims exist we currently assume they share this water; per-neighbour environments
and cross-sim track sharing are future work.

## The height hijack

The SS planes render at the environment's water height - the lowest enabled track water, each
track's authored tide being relative to that track's floor and lifted to world metres by
`visibleWaterHeight()` - and `SSWaterWorld` drives that same height into the region water height
store (`LLSurface`'s stock water object) every frame while Atmo owns the scene. Every consumer
that reads `getWaterHeight()` therefore follows the Atmo plane without being taught one by one:
underwater detection, the fog flips and the water clip plane, the camera's submerged test,
precipitation landing and the rain shadow floor, avatar swimming, the parcel overlay's waterline.

- Originals are captured at first hijack and put back on deactivate (or whenever no track
  carries a water plane), with one `LLWorld::updateWaterObjects()` so the stock hole/skirt
  slabs agree again. The stock set is suppressed while Atmo owns rendering, so the writes go
  straight to the water object - `LLSurface::setWaterHeight` would rebuild that stock set per
  region per change for nothing.
- An external write landing mid-hijack (a sim handshake, a god tool) is recognised - it is the
  one height on the region that is not the one we wrote - and kept as the new original, so
  deactivating never clobbers the sim's latest word.
- The height rides the `SSWaterWorld` rebuild signature, so re-authoring it rebuilds the SS set
  and re-drives the store without a region or camera move. The hijack runs before the signature
  is read, so a height change costs one rebuild, not two.

## The far-field squash

The tile ring is built out past the squash cap (`SS_WATER_SQUASH_CAP_FRAC` of `MAX_FAR_CLIP`,
the constant projection far plane) and the water vertex shader folds every vertex past the knee
(cap * 0.8) toward the cap edge along its exact ray - the same band `ssVolCloudV.glsl` runs for
the cloud field, and radial in full 3D: the far plane clips VIEW depth, so a horizontal-only
fold would leave the slant distance alone and the ocean would still cut out from under a camera
parked more than 2km up. Folding radially is what pulls the water up toward the eye as the
camera climbs, and since every vertex keeps its true ray the projected image is the true
ocean's - only the depth compresses. Direction is preserved, so the drawn image matches the
true positions and no triangle crosses the far plane (a sliced triangle rasterises black),
which is what let the ring extend freely where the old `reach` had to stop short.

The uniform is `(knee, cap, ring reach)` and lives on the shared water vertex shader
(`waterV.glsl`), written per frame by `LLDrawPoolWater::renderPostDeferred` keyed on the family
swap: stock water never wears Atmo geometry and gets zeros - passthrough - so nothing stale
survives a swap. The fog follows the drawn ray too: `vary_position` carries the drawn position
(the fragment shader's `calcAtmosphericVarsLinear` reads its distance from there) and the
vertex-stage atmospherics sample the drawn distance - for on-plane vertices the stock push is
exactly a mirror through the eye, so the mirrored drawn offset is written directly. Sampling
the true ray instead would blanket a sky build's ocean - kilometres of true distance - in full
fog. The water pass itself is no longer height-gated: the squash keeps the surface in the far
disc at every camera altitude, so there is no "too high up to draw" any more.

## SSEdgeWater tiling

Stock void water is nine stretched slabs (hole fillers plus an 8-piece skirt). SSEdgeWater is
instead a flat grid of 256x256m tiles - region-sized units - so each tile can later carry its own
waterline, per-neighbour height, or shoreline treatment without re-splitting geometry.

- Tiles are laid on the 256m global grid, anchored to the agent region's origin, and trimmed to a
  circle of radius `ring_reach = squash_cap + region_width / 2 + 256` around the region centre -
  past the squash cap, so every direction's outermost tiles fold onto the cap edge (see "The
  far-field squash" above). Where the old `reach` had to stop short of `MAX_FAR_CLIP` to keep
  triangles un-sliced, the fold now pulls every vertex back inside first, so the ocean reads to
  the horizon. Water is deliberately drawn past the draw distance (the water partitions set
  `mInfiniteFarClip`), so the projection far plane - not the draw distance - is what "up to the
  far clip" means here.
- A cell occupied by any connected region is skipped: the region gets a full-size `SSWater` plane
  at the environment's water height instead, and holes between regions get tiles naturally.
- Tiles use the environment's water height and the stock void slab
  convention: position Z at `256 + water_height` with Z scale 512, which puts the rendered quad at
  water height while the bounding slab reaches up for visibility culling.
- Rebuilds are keyed on a per-frame signature (applier active, agent region handle and water
  height, region set hash, water height sum, transparent-water setting, Atmo water height and
  validity) plus a dead-object sweep;
  a rebuild kills and recreates the whole set, which is cheap at a few hundred small objects and
  only fires on region changes, water height changes or the Atmo toggle.

## Deferred / known limits

- **The water_height spinner takes the full -10 km to +10 km by hand** (the slider keeps its
  near-surface dial) so a sky-themed build can put its ocean kilometres below the platform. The
  authored height is what the SS family renders and what the height hijack drives into the
  region store, so underwater detection, fog flips, precipitation landing and the rest follow it.
- **SSFarWater is a stub.** The squash carries the surface to the projection far plane's edge -
  the horizon the projection has - but true water beyond `MAX_FAR_CLIP` still needs eye-anchored
  geometry with a bespoke projection or shader trick (see the removed far-sea experiment in git
  history for constraints).
- Water shaders off (fixed-function water) renders the extended ring unsquashed - the far tiles
  then slice exactly as the old `reach` avoided. Atmo Magic is shader-driven everywhere else,
  so this only bites a deliberately broken graphics path.
- On very large var regions the camera can sit far from the region centre the tile circle is
  anchored to, thinning coverage in the camera's direction; the `region_width / 2` term in
  `ring_reach` keeps every direction covered past the cap from anywhere in the region, but the
  ring is not camera-centred. Camera-anchored tiling (with hysteresis to avoid rebuild churn) is
  the fix if it ever shows.
- Neighbour regions with their own Atmo environments, and tracks shared across sims, are future
  work; today every tile and every `SSWater` plane wears the agent's track.
