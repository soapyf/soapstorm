# Atmo Magic Landscape — synthesis design

Synthesis of four candidate designs (design_1…design_4) after adversarial review
(review_1…review_4). Provenance is cited per section: `[D1]`…`[D4]` for designs,
`[R1]`…`[R4]` for their reviews. Requirements R1–R7 are defined in 00_brief.md.

## 0. Verdicts absorbed

- **D1 (native local object)** is the chassis. Its thesis — a real, local,
  pcode-shared `LLVOVolume` subclass edited with stock tools and persisted by a
  reconcile funnel — survived review with repairable findings. `[R1: 6.5/10,
  "sound thesis, fixable with ~4 extra tagged injections"]`
- **D2 (headless renderer)** is rejected. Its load-bearing render slot was
  proven unimplementable as written, its headline 2 km scene renders nothing at
  stock far plane (`llviewercamera.cpp:340` — far plane is draw distance), its
  custom shader loses shadows/reflection probes/deferred features, and its
  spinner-only editing was judged second-class. `[R2]` Its verified survivors
  are absorbed: computed (not stored) region-locked transforms, publishing the
  applier's primary track index, HUD/snapshot gates.
- **D3 (author real, render local)** is rejected as the primary path (rez
  rights, prim quota, capture drift, arrival-selection breakage `[R3]`), and its
  unique capture trick was declined by the author. Its runtime substrate
  (client-only LLVOVolume in an infinite-far-clip partition) and its R4 fix
  (params-aware matching) are absorbed.
- **D4 (compound root)** is rejected outright: four independent fatal breaks in
  the editing loop `[R4]`, and its conceded real wins (one reconcile, one
  lifecycle, one re-anchor) are achievable with N independent roots under one
  manager at zero select-mgr surgery — its own reviewer's verdict. Its honest
  corrections are absorbed as facts: **there is no `LLVolumeImplMesh` in this
  fork** (per-child mesh fetch is `LLVOVolume::setVolume` → `gMeshRepo.loadMesh`
  → `notifyMeshLoaded`), and **per-object octree entries are correct, not a
  compromise** — one union bbox would be the actual culling hazard.

## 1. Architecture

### 1.1 Runtime object

`SSAtmoLandscapeObject : public LLVOVolume` — one live instance per landscape
record of the active track. Constructed directly (bypassing the pcode factory,
the SSWater precedent sswater.h:44-51), then registered with
`gObjectList.adoptViewerObject()` (llviewerobjectlist.cpp:2246) and
`gPipeline.createObject()`. Pcode stays `LL_PCODE_VOLUME` so every stock volume
mechanism engages unchanged: `setVolume` drives mesh fetch through
`gMeshRepo.loadMesh`, decode lands via `notifyMeshLoaded` (llmeshrepository.cpp:
4963), LOD ticking, faces-per-submesh, texture entries, draw pools, shadows,
reflection probes, octree. Zero rendering code of our own. `[D1, D3; R1, R3
verified the full chain]`

Construction (not the pcode factory) sets, before adoption:

- `LLVolumeParams` with sculpt type MESH and the record's asset UUID.
- Permission state from the record's captured metadata — modify/copy/transfer
  all true — so stock manipulator and menu permission checks (`permMove`/
  `permModify`/`permCopy`, llviewerobject.cpp:7198-7330) pass without touching
  their logic. This is the verified fix for R4's fatal-1. `[R4]`
- Name/description from the record (R2 carry-over).
- Phantom (no collision) via the stock flag; never physics.

**A local-content identity flag on LLViewerObject** — `ssIsLocalContent()` (bool
member, default false, set at construction) — is the single pivot every gate
tests. `[D1; R1 confirmed one flag is not sufficient for sends, but one flag is
sufficient as the predicate every site checks]`

### 1.2 Manager

`SSAtmoLandscapeWorld : LLSingleton` (ssatmolandscape.h/cpp) — the SSWaterWorld
shape `[D1/D3 family]`. Owns:

- The live object set (`std::vector<SSAtmoLandscapeObject*>`).
- **Track subscription**: the applier publishes its active track index
  (`primaryTrackIndex()` — small `<SS:Nexii>` addition to
  SSAtmoEnvApplier, same publication style as its billboard list `[D2's
  verified steal]`). A changed index triggers the UUID-adoption diff (§5).
- **Region re-anchor**: on agent-region change (same signal family
  SSWaterWorld already consumes), call `setRegion` + reposition each object per
  its record's mode (§7).
- **Reconcile loop** (§6): per frame, throttled, the persistence funnel.
- **Lifecycle**: kill-and-recreate on environment load/unload and on
  `SSAtmoEnabled` off (clearLandscapeObjects from the same llworld.cpp:138
  touchpoint that clears water); nothing exists when Atmo Magic is off.

N independent roots under one manager — not a compound root. `[R4 verdict]`

### 1.3 Editing (hybrid — author's choice)

- **Floater panel** (`SSPanelAtmoLandscape`, added to the env editor floater):
  the list. Per active track: entries (name, mesh, lock icon), drop target
  (R1), reorder, delete, track assignment, region-lock toggle per entry,
  select-in-world button. [Author decision: hybrid]
- **Stock build tools do all per-object editing**: click the mesh in-world →
  stock selection → build floater moves/rotates/scales it, face panel drops
  textures/PBR materials per face, General tab reads/writes name/desc. This is
  made to work by the gate+seed set in §4 — not by reimplementing any tool.
- The reconcile funnel (§6) syncs editor↔record both directions.

### 1.4 Mesh fetching

Pure stock, volume-coupled: the object's `LLVolumeParams` reference the asset
UUID; `setVolume` requests through the existing volume-coupled path with its
request dedupe (`mLoadingMeshes`) and 404 behaviour. **No standalone mesh-repo
extension** — D2's `MESH_REQUEST_LOD_STANDALONE` is dropped as unnecessary for
this architecture. `[D4 correction, R4-verified]`

## 2. Data flow

**Drop → record → hydration.** Drop mesh item on the floater list → `DAD_MESH`
cargo (llviewerassettype.cpp:81) → R1 gate (`getIsFullPerm()`, reject with a
message otherwise) → capture metadata (R2: name, desc, creator, last owner,
creation date) → new `SSAtmoEnvLandscape` record appended to the active track →
`SSAtmoLandscapeWorld::hydrate()` constructs + adopts the object → `setVolume`
starts the stock mesh fetch → faces arrive → TEs applied from record. Face
texture/material drops onto the object pass the same fullperm gate (standing
rule from the brief).

**Track crossing.** The applier's published track index changes → diff §5 →
adopt/create/kill in one frame, instant cut. `[R4]`

**Region crossing.** Region-change hook → re-anchor §7. Exactly one instance;
re-anchored, never per-region copies. `[D1]`

**Environment load/unload.** Load: manager already parses the notecard;
`SSAtmoLandscapeWorld` reconciles live set against the loaded asset's active
track (create/kill to match). Unload / `SSAtmoEnabled` off / leaving configured
parcel: `clearLandscapeObjects()` — same rule as water.

## 2a. Picking and culling (the repaired substrate)

- `PARTITION_LANDSCAPE` registered in the partition enum **immediately after
  PARTITION_VOLUME** with the push_back in matching order — the misalignment
  hazard R3 proved is called out in the diff comments. New
  `SSLandscapePartition` with `mInfiniteFarClip = true` (sky/water precedent)
  so scenery renders past draw distance — the premise of the 2048 m LOD
  stretch. `[D1/D3, repaired]`
- **Pick**: `LLPipeline::lineSegmentIntersectInWorld` enumerates partitions
  explicitly (pipeline.cpp:7350-7358) — `PARTITION_LANDSCAPE` must be added to
  that enumeration. This single line is what makes the object clickable and
  fixes R1's fatal-1; it is tagged and listed in §12. `[R1]`
- Far plane beyond draw distance is *intended*: landscape is atmosphere and
  behaves like the sky/water family, not like content. Draw-distance HUD gates
  (`sRenderingHUDs`, `gCubeSnapshot` guards) are inherited from the stock
  volume path automatically; documented for the custom pass in §6.

## 3. Schema (R7)

In ssatmoenvasset.h, per `SSAtmoEnvTrack`:

    "landscape": list of {
        "mesh_id":        UUID     // the asset, also the adoption key
        "name":           string   // from the dropped item, editable
        "desc":           string
        "creator":        UUID     // read-only record field
        "last_owner":     UUID     // read-only record field
        "created":        F64      // item creation date
        "pos_mode":       "locked" | "free"
        "locked_offset":  [F32,3]  // region-local X/Y/Z
        "free_global":    [F64,3]  // global position, mode "free"
        "rotation":       [F32,4]  // quaternion
        "scale":          [F32,3]
        "faces": array of {        // per-face TE snapshot, index = face
            "texture":   UUID
            "rpt":       [F32,4]   // repeats + offset
            "rot":       F32       // TE rotation
            "color":     [F32,4]
            "alpha_mode": S32
            "material":  UUID      // PBR RenderMaterial id, empty = none
        }
    }

Serialised through the existing pretty-XML path like every other field — hand-
editable, Bridge-copyable unchanged `[R7]`. Track-agnostic helpers live in
ssatmoenvasset.cpp following the existing per-struct `asLLSD/fromLLSD`
discipline.

**Notecard budget (corrected arithmetic):** a fully-loaded record ≈ 2 KB of
pretty XML (worst case: 8 faces × ~200 B + metadata). 16/track × 8 tracks would
be ~256 KB — over the 64 KiB notecard ceiling. The budget is therefore three
rules enforced at add time, surfaced in the floater: **16 records per track**,
**32 records per asset total**, and **sparse faces** (a face block is written
only when the face differs from the default TE; a plain-textured mesh serialises
~40 B per face). Realistic estates sit far under; the caps exist so the ceiling
is a friendly error, not silent truncation.

## 4. Drop and metadata (R1, R2)

`SSPanelAtmoLandscape::handleDragAndDrop` — the ssfloatertexturelist.cpp:175
pattern. Accept `DAD_MESH` only when the item
`getIsFullPerm()` (llviewerinventory.cpp:2482); otherwise a chat/`LLSD` notice
and no record. Metadata capture at drop into the record; name/desc become the
object's own, and the rest are read-only record fields surfaced in the floater
entry's tooltip. No second input path (author decision: drop-only).

## 5. Track crossing — UUID+params adoption (R4)

On the applier's published track index changing:

1. Diff live objects against the new track's landscape list.
2. Pair on `mesh_id`; **ties on equal `LLVolumeParams`** — same-mesh records
   with different volume params get `setVolume` re-applied (they are different
   render states), which re-hits the mesh repo's cached system volume; no fetch
   of already-loaded params. `[D1 pairing + D3's params fix + R3]`
3. Matched: keep the object instance; apply the new record's transform/TEs/name.
   No re-download, no rebuild pop. Unmatched-old: kill. Unmatched-new: create.
4. Presentation: instant cut, consistent with the preset/track idiom. `[R4,
   established idiom]`
5. Duplicates of one UUID within a track pair greedily nearest-first; a lost
   pairing rebuilds one object from cache — no user-visible cost. `[D1 risk,
   priced]`

## 6. Persistence — reconcile funnel

The applier's `put()` cache-and-diff idiom (ssatmoenvapplier.cpp:982-1013),
generalised into `SSAtmoLandscapeWorld::reconcile()`:

- Per live object, per throttled frame: build the record-shape state (position
  **in the record's frame** — region-local for locked records, so re-anchoring
  never reads as an edit `[D1]`; rotation; scale; name/desc; per-TE data),
  compare with the record, write changed fields into
  `SSAtmoEnvManager::editable()` (the working copy is the store `[D4]`; the
  existing save flow persists it).
- **Feedback-loop fix** `[R1 finding 4]`: adoption (record→object) and capture
  (object→record) are separate directions with separate stamps; the reconcile
  never re-applies a record to an object whose dirty stamp is newer than the
  record's. One rule, no mechanism gaps.
- Undo: stock build-tool undo restores client-side state from the select
  nodes' saved transforms **and** sends a server "Undo"/"Redo" message through
  `sendListToRegions` (llselectmgr.cpp:8001/8019) — the client-side restore is
  what the funnel detects; the server send dies in the funnel gate. Both halves
  verified in code. `[D1]`

## 7. Positioning (R6)

- **locked** (default): record stores region-local offset; on agent region
  change, `setRegion(newRegion)` + `setPositionRegion(offset)`. Z rides the
  shared sea-level datum. What you author in the editor is literally what every
  estate region renders.
- **free**: record stores `LLVector3d` global; region changes leave it put.
- Mode is per-record (floater toggle). Re-anchoring uses the stock
  `setRegion`/`mChildList`-free single-object path — N roots, no linkset math.

## 8. Failure modes

- **Mesh 404 / purged**: stock volume path marks unavailable; object renders
  nothing, floater entry shows a failed state; record survives (asset gap, not
  code gap — the house idiom). `[D1]`
- **No GetMesh cap / empty region**: fetch silently waits, as for any stock
  mesh. `[D4 verification]`
- **Track resolver inactive** (no sky, SSAtmoEnabled off): objects cleared.
- **Region cross while selected**: selection cleared on region change (same as
  stock crossing behaviour); reconcile is region-frame-aware so re-anchoring is
  not an edit.
- **Dropped item later deleted from inventory**: record holds the asset UUID;
  rendering is by asset id — unaffected, like any rezzed mesh.

## 9. Performance

- Idle: reconcile diff is O(records) with an early-out per object (position
  cached compare), throttled to ~4 Hz; LOD ticks are stock.
- Draw: stock pools; N records = N octree citizens with per-object frustum
  culling — strictly better than any compound bbox. `[R4 correction, kept]`
- Memory: vertex buffers are the stock mesh cache's, shared with any rezzed
  copy of the same asset by UUID.
- Notecard budget bounded by the 16/track cap.

## 10. Opt-in / off

`SSAtmoLandscape` (LLCachedControl bool, default false) under the existing
`SSAtmoEnabled` master — off means: no records hydrate, no objects exist, no
render type registered, no stock path changes behaviour. Matches the house
opt-in discipline and lets the author benchmark against vanilla.

## 11. Editing-path gating — the complete gate list `[R1, completed]`

All gated on `ssIsLocalContent()`; every site below is one early-return, every
one tagged `<SS:Nexii>`:

- `LLSelectMgr::sendListToRegions` (funnel for Object* messages)
- `LLSelectMgr::selectObjectOnly` inline ObjectSelect (llselectmgr.cpp:518-525)
- both deselect paths (985-1011, 1031-1037)
- both properties-request functions — `requestObjectPropertiesFamily`
  (6152) *and* `requestObjectPropertiesViaSelect` (6173), which packs its own
  inline ObjectSelect + ObjectDeselect directly (6183-6197) and never touches
  the funnel
- the two inline `MultipleObjectUpdate` sends (llselectmgr.cpp:9199, 9220)
- `LLToolGrab` ObjectGrabUpdate (lltoolgrab.cpp:731, 900)
- Firestorm direct senders: `fscommon.cpp:278` (ObjectPermissions) and `:302`
  (ObjectFlagUpdate), `fsfloaterimport.cpp:887`/`:1155`,
  `fslslbridge.cpp:1197`, plus the cmdline/animation-explorer sites
  enumerated in review_1's table — one-line guards each
- Pie menu: Delete → removes the record (+ funnel); Take/other server actions
  hidden for local content (menu builder filter)

TE/transform/name/desc sends (Object* messages at llselectmgr.cpp 3077-5531,
including `sendMultipleUpdate`'s MultipleObjectUpdate at 4903) all route
through the `sendListToRegions` funnel, so the funnel gate covers them;
everything listed above is precisely the traffic that bypasses it.

Selection plumbing that makes stock tools work on the objects:

- Construction-time permission flags (§1.1) — manipulators pass `[R4 fix]`
- Select nodes seeded from the record at the three verified node sites
  (llselectmgr.cpp:1068/1114/1211) so General tab, Inspect, texture drops read
  and write real values `[D1, R1-verified]`

## 12. Stock file diff list (complete; every injection `<SS:Nexii>`)

| File | Injection |
|---|---|
| llvovolume.h | `calcLOD()` virtual (one word) + protected `ssLODDistanceScale()` default 1.0 |
| llvovolume.cpp | one multiply at the `sDistanceFactor` line (1755) |
| llviewerobject.h/cpp | `ssIsLocalContent()` flag + accessor |
| llselectmgr.cpp | gate list (§11) + select-node seeding |
| lltoolgrab.cpp | grab/degrab gate |
| fscommon.cpp, fsfloaterimport.cpp, fslslbridge.cpp | one-line sender guards (§11) |
| pipeline.h (llpipeline) | `PARTITION_LANDSCAPE` + `RENDER_TYPE_LANDSCAPE` |
| pipeline.cpp | partition construction + pick enumeration (7350-7358) |
| llglsandbox.cpp (rect-select partition walk, ~234-297) | add `PARTITION_LANDSCAPE` so rubber-band selection sees the objects too — R4-fatal-3's mechanism, generalized from invisible-root to visible objects |
| llspatialpartition.cpp | `SSLandscapePartition` (infinite far clip) + registration; **render-type → pool mapping for the new partition is the one row that must be validated against the deferred pool walk during phase 1** (see §14) |
| llviewerdisplay.cpp | per-frame `SSAtmoLandscapeWorld::update()` at the existing Atmo touchpoint |
| llworld.cpp | landscape clear at the water-clear site (138) |
| ssatmoenvapplier.h/cpp | publish `primaryTrackIndex()` |
| ssatmoenvasset.h/cpp | `SSAtmoEnvLandscape` record + per-track list, LLSD round-trip |
| settings.xml | `SSAtmoLandscape` (bool) |
| skins …panel_ss_atmo_env.xml + new panel | Landscape tab |
| CMakeLists.txt (newview) | ssatmolandscape.cpp, ssatmolandscapeobject.cpp, sspanelatmolandscape.cpp |

New files: `ssatmolandscape.{h,cpp}` (manager + reconcile + diff),
`ssatmolandscapeobject.{h,cpp}` (the VO),
`sspanelatmolandscape.{h,cpp}` (+XUI xml).

## 13. Rejected, with the reason carved in

- **Headless renderer** — render-slot and far-plane findings are structural, not
  tuning `[R2]`.
- **Compound root** — its real wins are manager-level, not object-level `[R4]`.
- **Standalone mesh-repo requests** — unnecessary once objects are real volumes.
- **Authoring-phase temp rezzing** — declined by the author; drop-only input.

## 14. Known open risks (inherited honestly)

- **Render-type → pool mapping** for `PARTITION_LANDSCAPE` is the one
  load-bearing row not fully pre-verified: the deferred pool walk must include
  the new partition, exactly the class of trap R2 caught in the headless
  design. Phase 1's acceptance test is "landscape renders at 3 km" before any
  editing work begins.
- `sendListToRegions`-external senders can appear upstream; the gate list is
  the maintenance surface (documented in ssatmolandscape.cpp's header).
- Greedy duplicate-UUID pairing can momentarily orphan an instance on
  reordering; recovers next reconcile.
- The 8× LOD stretch is a starting curve; `DebugObjectLODs` works on the
  objects for tuning.

## 15. Addendum (2026-09-10): the drop never worked, and what replaces it

**Finding.** The inventory drop path in `SSFloaterAtmoEnv::handleDragAndDrop` accepts `DAD_MESH`
only, and nothing a user can drag is `DAD_MESH`: a mesh upload is re-tagged `AT_OBJECT` on
completion (`llmeshrepository.cpp`, "requested mesh asset type isn't actually the type of the
resultant object"), so it arrives as `DAD_OBJECT` and is rejected. Accepting `DAD_OBJECT` would not
help: an object item's asset UUID is the server-side prim blob, not a mesh asset id, so
`setSculptID(..., LL_SCULPT_TYPE_MESH)` could never resolve it. Every design and review in this
folder verified the `AT_MESH -> DAD_MESH` table entry without tracing what an upload produces.

**Direction (owner verdicts).** Design 3's capture is the path: rez the object, read what the
rezzed volume exposes, then derez. Verdicts recorded the same day:

- Whole prim linksets convert, not just mesh. The record becomes a root placement plus a parts
  array, each part carrying its volume params (mesh is the sculpt-id case, prims the
  path/profile case), root-relative transform and its own sparse face list. The object caps
  become a prim budget, and per-part records must stay compact for the notecard size limit.
- Full permissions are required on every prim in the linkset, from the owner mask (copy, modify,
  transfer), because the notecard hands every reader a working copy. The check is asynchronous:
  it waits for the whole linkset's object properties. Texture and material ids are already
  public in the object update, so they are not a new leak; mesh ids are the real exposure.
- Any prim with object contents (scripts, notecards, anything in its inventory) makes the
  conversion warn the user: a local object has no simulator and cannot run or hold contents.
  Client-side SLua (https://github.com/secondlife/slua/) is a possible future option, not a
  plan; for now object inventory items are invalid for local objects.
- Landscape objects draw a distinct selection silhouette (purple), hooked ahead of the
  parent/child colour branch in `LLSelectMgr::renderSilhouettes`, with one colour for root and
  children (nothing client-side can be unlinked) and the colour in the skin colour table beside
  the stock silhouette colours.

**Shipped today: limits for local content only.** `sslocalcontentlimits.h` gives objects flagged
`ssIsLocalContent()` a 2048 m scale ceiling and a 2048 m square placement area centred on the
object's own region (region-local X/Y in [centre-1024, centre+1024]); Z keeps the stock height
clamps. Applied in `llmanipscale.cpp` (per-object ceiling in the drag paths, selection-wide
ceiling in the drag-distance limits, local only when every selected object is local),
`llmaniptranslate.cpp` (the visible-region clip is replaced by the area clamp for local roots) and
`llpanelobject.cpp` (scale ceiling on refresh and send, position spinner range on refresh, area
clamp on send). Stock objects keep every stock clamp; `getState()` restores the stock spinner range
through `updateLimits()` on every refresh before the local override is applied.

**Build-tool verdicts (owner, 2026-09-10).** The possible list is the short one; the mechanism is
the menu builder filter that already hides Take for local content plus the build floater's per-tab
enable logic, with local content as a third case beside object and attachment.

- Works, and is wired to the record: position, rotation, scale and their copy/paste; the whole
  Texture tab including PBR; every shape parameter on the Object tab; name and description; Edit
  Linked Parts including moving a single part (writes its root-relative transform; the viewer
  enforces no link distance, only the sim does, so nothing needs unlocking for 2 km scenery).
- Keep, with a client-side meaning: Locked (guards the landscape against accidental edits);
  pathfinding attributes, Phantom and physics shape type (the census and the navmesh read them,
  and weather responds to them); Flexi and Light (schema additions); Build > Object > Duplicate
  (duplicates the record client-side); LOD show.
- Roadmap: Build > Object > Edit Particles and attached sound loops editable on local objects;
  Linden-style terrain in the void as a future landscaping option (land tools cannot act on a
  selected object today, so nothing to hide there).
- Hidden: every server action (Take variants, Buy, Pay, Return, Open, Touch, Sit, Wear, Attach,
  Put on, Add, Profile, Report Abuse, Block, Show in Region Objects/characters, Save as); every
  script item and the Content tab (local objects hold no inventory); Link and Unlink and the
  parent/child silhouette split; the General tab permissions block and Copy keys; Physical and
  Temporary; the Create tool (rezzes a sim object by definition).

**Shipped, same day (2026-09-10, later).** All five items from the verdicts above are implemented:

- Schema: `SSAtmoEnvLandscape` is a linkset record keyed by `mRecordId`, with `mParts[]`
  (`SSAtmoEnvLandscapePart`: volume params, root-relative offset and rotation, scale, sparse
  faces, light and flexi as LLSD). Part 0 is the root. Legacy one-mesh documents load as a
  one-part record and the legacy root keys are still written beside `parts`, so an older build
  renders the root mesh instead of rejecting the card. Prim budget:
  `SS_ATMOENV_MAX_LANDSCAPE_PARTS_PER_RECORD` (32) and `_TOTAL` (48), clamped on parse and
  refused on add.
- Runtime: one `SSAtmoLandscapeObject` per part; children are real `LLViewerObject` children
  (`addChild` + `setDrawableParent`), the root owns them by `LLPointer`, reconcile pairs by
  record id plus part count, capture writes each part back. Light and flexi apply as local
  parameter entries; `LLViewerObject::parameterChanged` no longer sends for local content.
- Convert selection: a state machine in `SSAtmoLandscapeWorld` (permissions on every prim of
  the family, contents check with a confirm dialog, capture, append, derez via
  `LLSelectMgr::selectDelete`, select the new root). The inventory drop is gone.
- Purple silhouette (`SSLocalContentSilhouetteColor`), and the build-tool and menu gating per
  the kept/hidden lists; Build > Object > Duplicate copies the record.

Known gaps carried forward: the derez goes through the prompting delete entry (it does not
prompt for an owned copy-able object, but RLVa can veto it silently, leaving both the record and
the original); face capture still omits glow, bump/shiny/fullbright, media and per-face GLTF
overrides; a seated avatar or non-volume child is skipped at conversion.
