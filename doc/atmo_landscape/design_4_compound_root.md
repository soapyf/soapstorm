# Atmo Magic Landscape — Design 4: the compound scene root

*One landscape of a track is one viewer-side object: `SSAtmoLandscapeRoot`, a local
`LLVOVolume` linkset adopted like the SSWater family, whose children are the landscape's
mesh instances. One thing to gate, one thing to re-anchor, one lifecycle, one reconcile
loop. The mesh children ride stock per-prim mesh plumbing.*

## 0. Two corrections to the strategy brief, up front

**There is no `LLVolumeImplMesh` in this fork.** The strategy text describes mesh
delivery as `LLVolumeImplMesh` "driven per child volume"; grep finds no such class
anywhere in indra/newview. Mesh handling lives directly in `LLVOVolume`:
`LLVOVolume::setVolume` (llvovolume.cpp:1229) detects `LL_SCULPT_TYPE_MESH` on the
volume's own sculpt params and drives `gMeshRepo.loadMesh(this, volume_params, lod,
last_lod)` (llmeshrepository.cpp:4482); the decoded `LLVolume` is delivered per waiting
volume by `LLMeshRepository::notifyMeshLoaded` (llmeshrepository.cpp:4963→4996-5002),
whose `mLoadingMeshes` map deduplicates concurrent requests for the same sculpt UUID
(llmeshrepository.cpp:4494-4514). The only `LLVolumeImpl` that exists is
`LLVolumeImplFlexible`. This is good news, not a setback: per-volume mesh fetch and
delivery is *already* per-prim machinery that every sculpted/mesh prim runs, so each
child volume carrying its own asset UUID needs **zero mesh infrastructure** — the
stock path fetches, dedupes across children sharing an asset, caches decoded LODs in
the ref-counted system volume (`LLPrimitive::getVolumeManager()->refVolume`, invoked in
`notifyMeshLoaded` at 4979-4993), and degrades to a prim proxy on 404
(llvovolume.cpp:1283-1289). The design reuses all of it and reimplements none of it.

**There is no "one octree entry", and we should not want one.** `LLDrawable::isRoot()`
is `!mParent || mParent->isAvatar()` (lldrawable.h:115), and `LLDrawable::getSpatialPartition`
(lldrawable.cpp:1208-1290) sends every *static* drawable — child prim or linkset root
alike — to `gPipeline.getSpatialPartition(mVObjp)`: the region's own `PARTITION_VOLUME`
octree (`LLVolumePartition`, llvovolume.cpp:5443). Only an *active* root drawable gets
an `LLVolumeBridge` with its own private octree (lldrawable.cpp:1252-1274). So a static
compound root still costs **N octree entries, one per child drawable — the same count
as N adopted root objects**. The pitch's "one octree entry regardless of mesh count" is
not a thing the pipeline can give a static landscape, and chasing it (forcing a bridge,
or collapsing children onto one drawable) would actively hurt: it would put a
region-spanning landscape's bounding box into a single octree bin and cull a scattered
mountain range as one unit. The design rejects the compound-bbox outright: every child
stays a real drawable in `PARTITION_VOLUME`, culling stays per-child, and the honest
wins are elsewhere — one selection gate, one reconcile loop, one lifecycle, one region
anchor, zero server-shaped objects, and a per-child-LOD topology for free. Section 9
quantifies this properly.

## 1. Architecture

### Files

| File | Contents |
|---|---|
| `ssatmolandscape.h/cpp` (new) | `SSAtmoLandscapeRoot`, `SSAtmoLandscapeChild`, `SSAtmoLandscapeWorld` (the driver singleton), the selection/gate helpers |
| `ssatmolandscapeeditor.h/cpp`, `skins/default/xui/en/floater_ss_atmo_landscape.xml` (new) | `SSFloaterAtmoLandscape` — the authoring floater with the child list, drop target, mode toggles and the undo stack |
| `ssatmoenvasset.h/cpp` (stock, `<SS:Nexii>`) | `SSAtmoEnvLandscape` + `SSAtmoEnvLandscapeRecord` structs, LLSD round-trip, schema constants |
| Stock files | see section 11 |

### Runtime shape

`SSAtmoLandscapeWorld : LLSingleton` — built exactly on the `SSWaterWorld` pattern
(sswater.h/cpp): it ticks once per frame from `llviewerdisplay.cpp`, immediately after
`SSWaterWorld::update()` at llviewerdisplay.cpp:1000, reads the same frame's resolved
track, and owns a signature-diff (`State`, `anyDead()`, `rebuild(bool)`) against the
track's landscape list. It owns exactly one live compound object:

- **`SSAtmoLandscapeRoot : public LLVOVolume`** — the anchor. Constructed directly
  (`new`, then `gObjectList.adoptViewerObject` at llviewerobjectlist.cpp:2246 — the
  SSWater path; the helper already exists, zero diff) with stock pcode
  `LL_PCODE_VOLUME`, then `gPipeline.createObject(root)`. It is scaffolding, not
  scenery: a 0.01 m box whose drawable carries the `LLDrawable::INVISIBLE` state
  (pipeline.cpp:3526 skips INVISIBLE drawables in the distance/LOD pass, and invisible
  drawables are not raycast targets), so it draws nothing and is picked by nobody. Its
  transform is the anchoring device: position = agent region's global origin, identity
  rotation, unit scale. It carries no record and no mesh — the whole landscape is its
  children, in stock linkset positions.
- **`SSAtmoLandscapeChild : public LLVOVolume`** — one per record. A real, standard
  `LLVOVolume` subclass whose volume params carry the record's mesh asset as the sculpt
  UUID with `LL_SCULPT_TYPE_MESH`, so per-child mesh fetch/LOD delivery runs entirely on
  the stock path. It overrides `calcLOD` (R3) and the `setTE*` virtuals (capture), and
  carries `mRecordIndex` + `mAssetID` so the reconcile can match it without a lookup
  table.

Children are stock linkset citizens. Creation sequence per child, mirroring what
`LLViewerObjectList::processObjectUpdate` does for a real linkset's parts:

```
SSAtmoLandscapeChild* child = new SSAtmoLandscapeChild(uuid, LL_PCODE_VOLUME, agent_region);
root->addChild(child);                 // mChildList + LLPrimitive::setParent (llviewerobject.cpp:1097)
gObjectList.adoptViewerObject(child);  // llviewerobjectlist.cpp:2246
gPipeline.createObject(child);         // creates the drawable AND sets drawable parent (pipeline.cpp:2147-2149)
child->setVolume(params, lod);         // mesh fetch through gMeshRepo.loadMesh
```

`LLPipeline::createObject` already routes `vobj->setDrawableParent(parent->mDrawable)`
when `vobj->getParent()` is set (pipeline.cpp:2147-2154), so the drawable hierarchy is
assembled by stock code with zero custom drawable surgery. `LLVOVolume::setDrawableParent`
(llvovolume.cpp:1876) then handles the active/static propagation and the
parent-relative `REBUILD_VOLUME` that all child prims go through. This is the
specific list of stock mechanisms reused wholesale, versus what we write:

| Mechanism | Source |
|---|---|
| Linkset parenting, child drawables, xform tree | `LLViewerObject::addChild` (llviewerobject.cpp:1113), `LLViewerObject::setDrawableParent` (1241), `LLVOVolume::setDrawableParent` (1876) — untouched |
| Per-child mesh fetch + LOD delivery + 404 handling | `LLVOVolume::setVolume` (llvovolume.cpp:1229), `LLMeshRepository::loadMesh` (4482), `notifyMeshLoaded` (4963), `notifyMeshUnavailable` (5010), the system-volume cache — all stock, all driven by the child's sculpt params |
| Pending-request dedupe across children sharing an asset | `mLoadingMeshes` map (llmeshrepository.cpp:4494-4514) — stock |
| Octree insertion/culling/raycast per child | `LLSpatialPartition::put` (llspatialpartition.cpp:948), `LLDrawable::getSpatialPartition` (1208) — stock; children are octree citizens exactly like static linkset prims are |
| LOD ticking per child | `LLDrawable::updateDistance` → `updateLOD` (lldrawable.cpp:962-966, pipeline.cpp:3544) — per-drawable, already |
| Selection machinery | `LLSelectMgr` wholesale, with one fake-valid annotation step (Firestorm precedent: fsareasearch.cpp:2224-2227) and a send gate |
| Region re-anchor | `LLViewerObject::setRegion` recursion into `mChildList` (llviewerobject.cpp:7438-7442) — one call re-anchors the whole hierarchy |
| Reconcile discipline | the applier's `put()` cache-and-diff idiom (ssatmoenvapplier.cpp:1041-1049) |

What we write instead: the record schema, the reconcile loop, the fullperm drop gate,
the transform/TE capture paths, the undo stack, the editor floater, and the LOD-stretch
override. That is the honest budget: the compound root is **less** new plumbing than it
looks (meshes, culling, linkset math and selection UI all come from stock), and much of
the strategy's "one thing" pitch is marketing for one reconcile loop and one selection
gate — which are still real wins, because N root objects would mean N independent
lifecycles to keep coherent across track crossings and estate copies.

### The one thing worth stating plainly

The root is **not** an octree/selection/pick delegate; it is a linkset root. The
children are octree citizens (one entry each, per child drawable — same as stock static
linkset children), pickable individually, drawable individually, LOD'd individually.
What the root buys: one anchor for the region math (R6), one lifecycle (R4/R7), one
selection gate, one reconcile loop, one kill path — everything the strategy list
claimed *except* the octree claim, which was always going to be wrong for a scattered
landscape and is better wrong-avoided than worked around.

## 2. Data flow

**Drop → record → hydrate → render.** The author drags a mesh inventory item onto
`SSFloaterAtmoLandscape`'s list (same `handleDragAndDrop` shape as
`SSTextureListCtrl::handleDragAndDrop`, ssfloatertexturelist.cpp:175). Cargo must be
`DAD_MESH` (llviewerassettype.cpp:81); anything else gets `ACCEPT_NO`. On drop:

1. `LLViewerInventoryItem::getIsFullPerm()` gate (llviewerinventory.cpp:2482) —
   modify-ok ∧ copy-ok ∧ transfer-ok. Non-fullperm → `ACCEPT_NO` plus a tooltip, and a
   popup when `drop` actually lands (the same refusal style the EEP-sky drop uses,
   per `doc/archive/atmo_magic_environment.md`).
2. Metadata copied off the item into the record (R2): name, description, creator id,
   last-owner id — creator and last owner are write-once read-only fields afterwards.
3. A new `SSAtmoEnvLandscapeRecord` is appended to
   `SSAtmoEnvManager::editable().mTracks[i].mLandscape.mRecords`, with the drop's
   *current agent-region-local* transform for a locked record (mode defaults locked).
4. The editor bumps the reconcile signature. `SSAtmoLandscapeWorld::update()` diffs,
   news a child, links it, `gPipeline.createObject`, `setVolume(LLVolumeParams with
   sculptID = asset UUID, LL_SCULPT_TYPE_MESH, profile/path defaults)` at the record's
   LOD target → stock fetch via `GetMesh`/`GetMesh2` caps (constructUrl,
   llmeshrepository.cpp:1464; entry 4796-4799) keyed on the asset UUID. Delivery lands
   via `notifyMeshLoaded` → `LLVOVolume::notifyMeshLoaded` → rebuild. Render path,
   pools, shaders, shadows: all stock volume rendering.

**Track crossing (R4).** The applier already resolves the primary track each frame;
the design adds `S32 mLastPrimaryTrack` + `lastTrackIndex()` to `SSAtmoEnvApplier`
(two-line tagged injection). `SSAtmoLandscapeWorld::update()` diffs its State (active,
track index, region handle, record-list revision, dead sweep). On a track change, the
reconcile diffs the live children against the new track's records **by asset UUID**:
same UUID → keep the child VO, apply the new record's transform + TE (a same-UUID
`setVolume` re-refs the cached system volume — no fetch, no pop, no decode); only-in-old
→ kill; only-in-new → create. Presentation is an instant cut, matching the "a preset
can't be interpolated" idiom: transform adoption is one frame, no tween.

**Region crossing (R6).** One region-handle change in the signature →
`SSAtmoLandscapeWorld::reanchor()`:

1. `root->setRegion(newRegion)` — `LLViewerObject::setRegion` (llviewerobject.cpp:7417)
   cascades to every child via the `mChildList` recursion (7438-7442), resetting each
   drawable's partition membership into the new region's `PARTITION_VOLUME` octree.
2. Reposition the root: global position = new region origin (identity rot/scale).
   Locked children's parent-relative transforms are *already* region-local, so they
   inherit the re-anchor through the xform tree untouched; `updateDrawable`'s
   `markMoved` path (llviewerobject.cpp:6770-6791) moves them into the new region's
   octree.
3. Free-mode children: recompute `parent_relative = global − root_global` (root carries
   identity rotation and unit scale, so this is a subtraction) and write the child's
   local transform — the one place the compound frame earns its keep: the re-anchor is
   the root's position, every other child follows stock linkset math.

No per-region copies are ever built — one instance, moved (R6's explicit contrast with
the SSWater family, which rebuilds per-region planes on purpose).

**Load/unload.** `SSAtmoEnvManager` load → asset parsed → next frame the reconcile
sees `mHasAsset` + enabled and hydrates the resolved track's records. Unload, or
`SSAtmoEnabled` off, or applier going inactive: `clearLandscape()` kills the set (child
VOs first via `gObjectList.killObject`, then the root — children are independent
gObjectList entries and must each be killed; `SSWaterWorld::clearWaterObjects`
sswater.cpp:252-267 is the pattern). The notecard asset itself never changes on unload.

## 3. Schema

Added to `SSAtmoEnvAsset`, per track (a new member on `SSAtmoEnvTrack`):

```
struct SSAtmoEnvLandscapeRecord
{
    LLUUID     mAssetID;          // the mesh asset; the match key for R4 adoption
    std::string mName;            // from the dropped item (R2)
    std::string mDescription;
    LLUUID     mCreatorID;        // read-only capture
    LLUUID     mLastOwnerID;      // read-only capture (R2)
    S32        mPositionMode;     // SS_ATMOENV_LANDSCAPE_LOCKED / SS_ATMOENV_LANDSCAPE_FREE
    // locked: region-local X/Y in [0,256], Z in shared sea-level metres
    // free: global position, survives region crossings
    LLVector3  mLockedPos;        // region-local when locked (else unused)
    LLVector3d mGlobalPos;        // global when free (else ignored)
    LLQuaternion mRotation;
    LLVector3  mScale;
    LLSD       mTES;              // array of per-face texture/material records, ≤9 entries
};

struct SSAtmoEnvLandscape
{
    std::vector<SSAtmoEnvLandscapeRecord> mRecords;
    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};
```

`SSAtmoEnvTrack` gains `SSAtmoEnvLandscape mLandscape;`, serialized as one new key:

```xml
<track>
  <map>
    <key>name</key><string>Ground</string>
    ...existing keys unchanged...
    <key>landscape</key>
    <array>
      <map>
        <key>asset_id</key><uuid>a1b2...</uuid>
        <key>name</key><string>Great Peak</string>
        <key>desc</key><string></string>
        <key>creator</key><uuid>...</uuid>
        <key>last_owner</key><uuid>...</uuid>
        <key>mode</key><string>locked</string>   <!-- or "free" -->
        <key>pos_local</key><array><real>12.5</real><real>200.0</real><real>940.0</real></array>
        <key>pos_global</key><array><real>256091.0</real><real>256128.0</real><real>940.0</real></array>
        <key>rot</key><array>4 reals (quaternion x,y,z,w)</array>
        <key>scale</key><array>3 reals</array>
        <key>tes</key><array>
          <map>
            <key>face</key><integer>0</integer>
            <key>texture</key><uuid>...</uuid>
            <key>color</key><array>4 reals</array>
            <key>scale</key><array>2 reals</array>
            <key>offset</key><array>2 reals</array>
            <key>rot</key><real>0.0</real>
            <key>glow</key><real>0.0</real>
            <key>material</key><uuid>...</uuid>   <!-- PBR override slot -->
          </map>
        </array>
      </map>
    </array>
  </map>
</map>
```

Rules: `SS_ATMOENV_VERSION` bumps to 2 (the version marker `sd["version"]` round-trip
in `SSAtmoEnvAsset::asLLSD`/`fromLLSD` already tolerates old documents — landscape
keys absent from an old asset simply leave the list empty, matching the
missing-field tolerance every other block uses). Records tolerate missing fields on
`fromLLSD` (asset UUID null → skipped with a warning). TE records beyond the mesh's
face count on hydrate are applied up to the count the mesh actually has. Cap:
`SS_ATMOENV_LANDSCAPE_MAX_CHILDREN = 64` per track (an `LLCachedControl` debug setting,
`SSAtmoLandscapeMaxChildren`, clamped 1–256, following the fsrezqueue
`LLCachedControl` statics convention); the drop target refuses past the cap.

## 4. Editing UX

The build tools are the editing surface; the floater is the bookkeeper.

**Selection proxying.** Children are ordinary pickable volumes — the pipeline raycast
returns the child VO directly, so hover, pie menu and click all already know which mesh
the author touched. Two stock pick entry points get the redirect:

- `LLToolSelect::handleObjectSelection` (lltoolselect.cpp:82-228, the click-select
  funnel): when the picked object is an `SSAtmoLandscapeChild`, call
  `LLSelectMgr::selectObjectOnly(child)` — never
  `selectObjectAndFamily`, regardless of `EditLinkedParts`/`mIgnoreGroup`, so a click is
  always per-child and never drags the whole landscape's family in.
- `LLToolSelectRect::handlePick` (lltoolselectrect.cpp:75-82, the drag-box tool): same
  redirect, so rubber-banding selects the caught children individually.

The redirect is the "selection proxying": the child VO *is* the editor state — there is
no second editor state to route to. Selecting the root through the world is impossible
by construction (invisible, non-picking); the editor's "select all" row action calls
`selectObjectAndFamily(root)` if the author wants the whole set at once.

**Fake-valid select nodes.** Without a sim, no `ObjectProperties` reply ever arrives,
and every TE/perm panel functor gates on `node->mValid` (llselectmgr.cpp:3459+). So the
selection hook annotates nodes at creation: one `<SS:Nexii>` injection in
`LLSelectMgr::addAsIndividual` (llselectmgr.cpp:1106 — both `selectObjectOnly` and
`addAsFamily` funnel through it) calling `SSAtmoLandscapeWorld::annotateNode(nodep)`:
`node->mValid = true`, full `mPermissions` (PERM_ALL, agent-owned — the author owns
these records by construction), `mName`/`mDesc`/`mCreatorID`/`mLastOwnerID` from the
record. This is the exact pattern Firestorm itself uses to select objects without sim
properties (`fsareasearch.cpp:2224-2227`). Result: the stock build floater, texture and
face panels, inspect and the manipulators all behave as if this were a full-perm
in-world object the agent owns.

**Traffic gate.** Every selection-driven send for an SS landscape object becomes a
no-op — and the *commit* for several of them:

- `LLSelectMgr::sendListToRegions` (both overloads, llselectmgr.cpp:5940/5949): the
  funnel for `ObjectSelect`, `ObjectDeselect`, `MultipleObjectUpdate`, `ObjectName`,
  `ObjectDescription`, `ObjectPermissions`, `ObjectOwner`, `ObjectDelete`, `ObjectDeRez`,
  `ObjectDuplicate`, `ObjectLink`, `ObjectBuy` — SS nodes are filtered out of the send
  queue; for `MultipleObjectUpdate` nodes the gate instead calls
  `SSAtmoLandscapeWorld::captureTransform(node)` (position/rotation/scale into the
  record, undo snapshot, revision bump); for `ObjectName`/`ObjectDescription` it
  captures name/desc the same way. This is the commit point for translate/rotate/scale
  drags: `LLManip` already applied the final transform to the child VO before
  `sendMultipleUpdate` fires (llselectmgr.cpp:4892).
- `selectObjectOnly`'s inline `ObjectSelect` packet (llselectmgr.cpp:516-525): skipped
  for SS objects (also avoids the `getRegion()` dereference path — children do carry
  the agent region, but the packet must not go out with localID 0).
- The two inline `ObjectDeselect` sends (985, 1031): skipped.
- `requestObjectPropertiesFamily` (6156) and `requestObjectPropertiesViaSelect`
  (6173-6198): skipped for SS objects — nothing should ever ask the sim about them, and
  the fake-valid node means nothing waits for a reply.
- `canSelectObject` (8128): early-true for SS landscape objects — they are the agent's
  own full-perm constructs by definition, and the `SelectOwnedOnly` path would
  otherwise refuse them (`permYouOwner` on a local object is false).

Non-selection stock paths that would otherwise misbehave stay stock: `LLManip` works
off the (now valid) nodes; `EditLinkedParts` is irrelevant because our redirect never
enters linkset mode; the stock right-click menu's Delete/Take still fire their packets,
which the gate eats, so the worst case is a silently-ineffective menu item on an object
that should only be removed through the landscape editor — and the editor's own
Remove/Duplicate/Take-into-record actions cover the useful cases.

**Editing surface.** The floater (`floater_ss_atmo_landscape.xml` + `SSFloaterAtmoLandscape`,
in the shape of `SSFloaterTextureList`/`ssatmoenvapplier`-era floaters) carries: the
track selector (defaults to the track the camera occupies), the child list (name,
asset UUID, mode chip, loaded/missing state), a drop target strip (DAD_MESH only,
fullperm gate, cap message), per-row Select (routes to `selectObjectOnly(child)`),
Remove, Duplicate, a mode toggle per record, and coordinate readouts — region-local for
locked (X/Y 0–256, Z free), global for free. Name/desc editing happens in the stock
build floater's General tab: the `ObjectName`/`ObjectDescription` sends are gated and
captured into the record at the same commit point.

**Undo.** `SSFloaterAtmoLandscape` owns a snapshot stack of the selected track's whole
record list (`SSAtmoEnvLandscape` is small structs; 64 records is trivial). Pushed at
every commit (drop, remove, duplicate, mode change, transform commit, TE commit,
name/desc commit); Ctrl+Z / the floater's undo button pops one. Stock Edit-menu undo is
left alone — it cannot work (in-world object undo is server-side), which is stated here
rather than half-implemented: undo for landscape edits lives in the landscape editor,
full stop.

## 5. Persistence

The manager's working copy is the single live store, and every mutation is an
immediate write into `SSAtmoEnvManager::editable()`. There is no separate save step and
no event hook: the *live asset* is the persistence point, the notecard write is the
existing `saveNotecard`/`updateExistingNotecard` flow
(ssatmoenvmanager.cpp:690-724) unchanged, and `isModified()` (which diffs
`mWorking.asLLSD()` against baseline) automatically covers landscape fields. Load
restores through the same round-trip: the reconcile loop hydrates children from
`asset.mTracks[i].mLandscape` whenever its signature (active state, track index, region
handle, record-list revision — a hash of the record list) moves, exactly the
`put()`-cache-and-diff discipline the applier uses (ssatmoenvapplier.cpp:1041-1049).
One reconcile loop, one signature, and the child count is just a list length — adding a
mountain costs one `mRecords.push_back`, not a new lifecycle.

Region-locked vs free is carried *per record* (R6), because the compound root is the
re-anchor point but each child chooses its own mode:

- **locked**: `mLockedPos` is region-local (X/Y clamped 0–256, Z free metres in the
  shared sea-level datum). With the root anchored at the region origin with identity
  rotation and unit scale, the child's *parent-relative transform is the record*
  verbatim — locked children never need re-computation, which is the whole reason the
  root anchor is pinned at region origin: the authored region-local numbers and the
  linkset-local numbers are the same numbers, so "what you author is what every region
  gets" is a rounding-free identity.
- **free**: `mGlobalPos` (LLVector3d). Parent-relative transform is recomputed on every
  re-anchor as `rel = global − root_global` (root has identity rotation/scale, so no
  matrix inversion needed); the child then keeps its parent-relative transform while
  the root re-anchors beneath it. Editor shows the global readout.

Re-anchor itself (R6, "exactly one instance... re-anchored, not per-region copies"):
on region change, `root->setRegion(newRegion)` — which cascades through every child
(llviewerobject.cpp:7438-7442) — followed by the root's reposition; children follow the
xform tree, partitions move via the standard `markMoved` path. One call re-anchors any
number of children; free children get their one recomputation pass.

## 6. LOD strategy

R3 rides the per-child hook. `LLVOVolume::updateLOD` is already virtual
(llvovolume.h:247) but the workhorse `calcLOD` is not (llvovolume.h:433); the design
makes `calcLOD` virtual — a one-line `<SS:Nexii>` diff in llvovolume.h — and
`SSAtmoLandscapeChild` overrides it with the stock algorithm plus one inserted line:
`distance *= SS_ATMOENV_LANDSCAPE_LOD_RANGE (1/8)` before the `sDistanceFactor` multiply.
Everything downstream (the near-boost ramp, `sLODFactor`, the FOV-zoom factor, the
`DebugSelectionLODs` and HUD special cases) is preserved verbatim, so the user's
Mesh-detail preference still scales the stretch and `DebugObjectLODs` still annotates
per child (each child has its own drawable, hence its own `mDistanceWRTCamera` and
`mLOD` — the per-child LOD story needs no new plumbing at all; it falls out of each
child having a drawable). The stretch applies to the *effective distance* only, so the
fetched LOD (`mLOD` → `LLVolumeLODGroup`) stretches identically: LOD-0-into-LOD-1
switches land around 2048 m instead of 256 m. The same constant — an
`LLCachedControl<F32>`, `SSAtmoLandscapeLODStretch`, default 8.0, clamped 1–16 — is a
debug setting so the review pass can measure what 8× buys and break it easily.

Because the override re-derives the stock algorithm, it is written against
`calcLOD`'s shape as of this branch and annotated as the single place LOD math forks;
stock volumes are untouched (virtual dispatch changes nothing for them).

## 7. Culling/draw distance

No new culling policy is needed — and that is the finding worth leading with. Because
each child is a real drawable in the region's static `PARTITION_VOLUME` octree
(`LLSpatialPartition::put`, llspatialpartition.cpp:948-972; static volumes route through
`LLDrawable::getSpatialPartition` → `gPipeline.getSpatialPartition(mVObjp)`), a
region-spanning landscape is culled **per child**: a mountain you face is in a near bin,
a skyline element behind you is not visited. The "one huge bin" hazard belongs to the
naive reading of the compound strategy — one drawable wearing N meshes, or one octree
entry carrying the union bbox — and the design rejects it explicitly. The compound root
is *not* a spatial authority; it is a lifecycle/selection/anchoring device, and its own
drawable is INVISIBLE and empty (it contributes one negligible octree entry at the
anchor).

Draw distance: children render through `RENDER_TYPE_VOLUME` and inherit the draw
distance discipline stock meshes have — distant scenery pops in at draw distance. That
is the correct default for "atmosphere, not content": a 2048 m draw distance already
covers the LOD stretch's effective range, and terrain/sky already read past it. No
exemption, no `mInfiniteFarClip` games, no exceptions — the same rule water's ring
needed is one this geometry does not, because nothing here is drawn past the far clip
by design intent; a skyline that wants horizon presence should be authored near the far
plane, not past it. (If the author wants horizon scenery, the mesh itself is built
large; we do not extend draw distance for it.)

The `OctreeStaticObjectSizeFactor`-scaled bin assignment follows each child's
`getBinRadius` (llvovolume.cpp:4812) — normal per-prim behavior, no compound exceptions.

## 8. Failure modes

| Failure | Behaviour |
|---|---|
| Mesh asset 404 / purged | Stock path: `LLVOVolume::setVolume` detects the dead asset (`is404`, llvovolume.cpp:1237-1289), swaps the sculpt ID to null and renders the prim proxy with the Inv_Mesh icon; `LLMeshRepository::notifyMeshUnavailable` (llmeshrepository.cpp:5010) marks the system volume unavailable. Root and siblings unaffected; the editor row shows "missing" (read via `LLSculptIDSize`/`isMeshAssetUnavaliable`) with a re-fetch button. The record is never mutated by failure. |
| No region caps (`GetMesh` URL empty) | `loadMesh` request never leaves; same placeholder behaviour; retried on the next region/track signature change. |
| Track resolver inactive (applier off, no asset) | No root is built; `SSAtmoLandscapeWorld::update()` early-outs on `!SSAtmoEnvApplier::isActive()` — the same gate SSWaterWorld uses. |
| Track with landscape but resolver returns an out-of-range index | Index clamped like the applier does (ssatmoenvapplier.cpp:183-187); if the track's landscape list is empty, no root exists. |
| Region teardown / disconnect kills objects underneath us | `anyDead()` sweep (sswater.cpp:270 precedent) detects dead VOs and the rebuild recreates from records — meshes are ref-counted in the system volume cache, so it is a transform re-apply, not a re-fetch. |
| Child selected during a track crossing that replaces it | `SSAtmoLandscapeWorld` forces `deselectObjectAndFamily(child)` before killing replaced children — selection flushes before the VO dies, rather than leaving dangling select nodes (stock's `findNode` dead-object tolerance covers the residual window). |
| Empty region (no caps, no agent region) | Skip and retry next frame — identical to SSWaterWorld's region-null guard. |
| Track record list edited while Atmo disabled | Nothing hydrates (world inactive); edits persist in the asset for the next enable. |
| Estate copy to a region with no `GetMesh` cap | Children fall back to the placeholder box; scenery degrades per child, never wholesale. |

## 9. Performance

**Idle, per frame:** the reconcile is a struct-compare against a cached `State` plus a
dead-object sweep over ≤64 children — the SSWaterWorld signature pattern, sub-µs when
clean. Children are static: they are absent from `mActiveObjects` (no per-frame
`idleUpdate`), get the amortized `updateApparentAngles`/`mDistanceWRTCamera` pass every
drawable already gets, and nothing else. No per-frame reconcile work while nothing
changes — the `put()` discipline is what keeps it that way.

**Memory:** identical to N viewer objects — one `LLVOVolume` + one `LLDrawable` + faces
per child — minus the structures a server-backed object would carry (created lists,
update bookkeeping) that never existed here. The mesh volumes are refcounted system
volumes shared with anything else displaying the same asset.

**Octree:** honest ledger. Each static child drawable is its own `PARTITION_VOLUME`
octree citizen — N entries, same as N root objects would cost. The savings compound
elsewhere: one `setRegion` call re-anchors all children (root cascade,
llviewerobject.cpp:7438-7442), one reconcile diff instead of per-instance bookkeeping,
one selection gate instead of N, one gObjectList lifecycle. During an edit drag the
root briefly goes active and the whole linkset hops into one `LLVolumeBridge`
(llvovolume.cpp:5454) — transient, stock behaviour for any edited linkset.

**Draw ordering/culling:** unchanged from stock static linksets — per-child drawables
batch into the normal pools (opaque/simple/norm/spec/pbr/alpha by face), culled and
sorted per drawable. A scattered landscape with 40 children behaves, in the render
path, exactly like a 40-prim static linkset — which is the baseline stock linksets
already meet on every parcel.

**Worst case:** 8 tracks × 64 children = the cap set only renders one track's set at a
time (instant cut per R4), so the worst live set is 64 children + 1 root. Meshes are
refcounted system volumes; duplicates of one asset share both the volume and the
in-flight HTTP request.

## 10. Opt-in/off behaviour

`SSAtmoEnabled` off, or asset unloaded, or applier inactive: `SSAtmoLandscapeWorld::update()`
kills the whole set (children, then root), resets its State, and returns — the same
single-exit pattern `SSWaterWorld` uses, and no reconcile, no octree entries, no
selection. A selection holding landscape children is flushed by the existing
`LLSelectMgr::validateSelection` path (the SS early-true in `canSelectObject` is itself
gated on the objects existing, and dead objects deselect naturally). Every stock
injection is written to no-op on non-SS objects, and no SS object exists while the
feature is off, so the stock path is bit-identical with the master switch off — the
same guarantee the SSWater family relies on. Leaving a configured parcel mid-edit
keeps the landscape (it is keyed to the *track*, not the parcel, like the rest of the
environment); leaving the estate/unloading the environment kills the set.

## 11. Stock file diff list

Every injection wrapped in `<SS:Nexii>` … `</SS:Nexii>`; new identifiers all `SS`
-prefixed; no build, no commits.

| Stock file | Injection |
|---|---|
| `llvovolume.h` | `calcLOD()` made virtual (llvovolume.h:433) — one line, the R3 anchor |
| `llselectmgr.cpp` | six gates: the `sendListToRegions` funnel (5940 + the 5949 overload; skip SS nodes, capture transforms/names on gated `MultipleObjectUpdate`/`ObjectName`/`ObjectDescription`), the inline `ObjectSelect` in `selectObjectOnly` (516-525), the two inline `ObjectDeselect` sends (985, 1031), `requestObjectPropertiesFamily` (6156), `requestObjectPropertiesViaSelect` (6183-6197), and an `SS` early-true in `canSelectObject` (8128). Plus the one annotation call in `addAsIndividual` (1114, after node creation). |
| `lltoolselect.cpp` | pick redirect: an SS child routes to `selectObjectOnly` (`handleObjectSelection`, 82-228) |
| `lltoolselectrect.cpp` | same redirect for drag-box selection (`handlePick`, 75-82) |
| `lltooldraganddrop.cpp` | `dad3dApplyToObject` fullperm gate for texture/PBR-material drops onto SS landscape faces (the R1 consistency rule) |
| `ssatmoenvapplier.h/cpp` | `mLastPrimaryTrack` + `lastTrackIndex()` accessor, stamped from the resolved blend in `apply()` |
| `llviewerdisplay.cpp` | one line after `SSWaterWorld::update()` (1000): `SSAtmoLandscapeWorld::getInstance()->update()` |
| `app_settings/viewer.xml` (settings) | `SSAtmoLandscapeLODStretch` (F32, 8.0, clamp 1–16), `SSAtmoLandscapeMaxChildren` (S32, 64) — both read as `LLCachedControl` statics, fsrezqueue style |
| xui | `floater_ss_atmo_landscape.xml` (+ en strings; no notifications — the floater carries its own rejection tooltips, matching `SSTextureListCtrl`'s pattern) |

New files (all `viewerlgpl` headers, `ss` prefix, style matching `fsrezqueue.cpp`):
`ssatmolandscape.h/cpp`, `ssatmolandscapeeditor.h/cpp`, plus the schema additions in
`ssatmoenvasset.h/cpp` and the small LLSD round-trip for records. No new partitions,
pools, shaders, or factory registrations — pcodes are stock (`LL_PCODE_VOLUME` for both
root and children), exactly like the water family's shared-pcode argument.

## Known risks, stated plainly

- **The pitch's own headline oversold the octree.** This design runs with the truth
  (per-child octree entries, identical to N roots) and keeps the wins that are real —
  lifecycle, selection, reconcile, anchoring. If a reviewer's synthesis wants the
  "one octree entry" claim, it does not exist in this pipeline's stock mechanics for
  statics and should not be sold.
- **`calcLOD` virtualization forks the LOD body once.** Acceptable: one override,
  annotated, and stock dispatch keeps every other volume pristine.
- **Fake-valid select nodes are a precedent-loaded trick** (Firestorm itself does it
  in `fsareasearch`), but they are load-bearing for the stock build panels; a stock
  update to those functors' expectations would need a re-check.
- **Context menus overkill**: dead menu items (Take, Sell, Pay...) on SS children are
  eaten by the gate; trimming the menus properly would want enable-callback
  injections in `llviewermenu.cpp` that are not worth their surface area in v1 — the
  design hides the two that mislead most (Delete, Take) by leaving them as no-ops and
  noting the wrinkle.
- **The root being an invisible volume in the octree is one extra entry per landscape**
  — negligible, and the alternative (no root drawable at all) loses the linkset
  machinery that makes the whole design cheap.
