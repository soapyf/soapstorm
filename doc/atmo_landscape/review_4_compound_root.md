# Review: Design 4 (compound root)

Adversarial review of `design_4_compound_root.md` against the actual code in this
worktree. Every codebase claim below was read, not trusted. R1–R7 checked one by one.

## Verification of codebase claims

| Claim (design §) | Verdict | Correction / note |
|---|---|---|
| No `LLVolumeImplMesh` in this fork; only `LLVolumeImplFlexible` exists (§0) | CONFIRMED | Only `LLVolumeImplFlexible` (llflexibleobject.h:71); no `LLVolumeImplMesh` anywhere. |
| `LLVOVolume::setVolume` at llvovolume.cpp:1229 detects `LL_SCULPT_TYPE_MESH` and drives `gMeshRepo.loadMesh` (4482) (§0) | CONFIRMED | Signature `loadMesh(LLVOVolume*, params, lod, last_lod)` at llmeshrepository.cpp:4482 ✓. |
| 404 → sculpt null + Inv_Mesh icon prim proxy (llvovolume.cpp:1283-1289) (§0, §8) | CONFIRMED | llvovolume.cpp:1283-1289; `is404` detection at 1247-1251 via `getActualMeshLOD` returning -1. |
| `mLoadingMeshes` dedupe per (lod, mesh_id), volumes appended (4494-4514) (§0) | CONFIRMED | llmeshrepository.cpp:4494-4514. |
| `notifyMeshLoaded` 4963→4996-5002; refVolume system-volume cache 4979-4993 (§0) | CONFIRMED | 4963 entry, 4981 refVolume, 4996-5002 per-volume notify. |
| `notifyMeshUnavailable` 5010 (§1 table) | CONFIRMED | |
| `LLDrawable::isRoot()` = `!mParent || mParent->isAvatar()` (lldrawable.h:115) (§0) | CONFIRMED | Exact. |
| `getSpatialPartition` 1208-1290; statics → `gPipeline.getSpatialPartition`; only active root gets `LLVolumeBridge` (lldrawable.cpp:1252-1274) (§0) | CONFIRMED | Active *children* follow `getParent()->getSpatialPartition()` (1280). Partitions are per-region: `new LLVolumePartition(this)` at llviewerregion.cpp:729. |
| "pipeline.cpp:3526 skips INVISIBLE drawables in the distance/LOD pass" (§1) | IMPRECISE | 3526 gates `setVisible` (render visibility) only. The `updateDistance` block (pipeline.cpp:3532-3547) runs regardless of INVISIBLE. End result (root draws nothing) is right; the stated mechanism is wrong, and LOD distance still ticks for the root. |
| "invisible drawables are not raycast targets" (§1) | CONFIRMED (mechanism unstated) | Not because of INVISIBLE in `LLVOVolume::lineSegmentIntersect` (llvovolume.cpp:4962 checks `mbCanSelect`/dead/render-type, not INVISIBLE) but because the pick visitor gates on `drawable->isVisible()` (llspatialpartition.cpp:3961) and INVISIBLE blocks `setVisible` (pipeline.cpp:3526). Behaviour holds; the design should cite 3961. |
| `LLViewerObject::setRegion` recursion through `mChildList` (llviewerobject.cpp:7417, 7438-7442) (§1, §2, §5) | CONFIRMED | 7417-7451; recursion 7438-7442; `updateDrawable(false)` per child → `markMoved` (llviewerobject.cpp:6770-6791) ✓. Also `addToCreatedList(getLocalID())` — harmless for localID 0 (guarded `local_id > 0`, llviewerregion.cpp:3061-3067). |
| `LLPipeline::createObject` sets drawable parent when `vobj->getParent()` (pipeline.cpp:2147-2154) (§1) | CONFIRMED | pipeline.cpp:2147-2154. |
| `LLVOVolume::setDrawableParent` (1876) handles active/static + parent-relative REBUILD_VOLUME (§1) | CONFIRMED | llvovolume.cpp:1876-1900. |
| `LLViewerObject::addChild` (1113) / `setDrawableParent` (1241) (§1) | CONFIRMED | addChild at 1113 (setParent at 1097); design's code block conflates the two citations slightly but both functions exist as cited. |
| `gObjectList.adoptViewerObject` llviewerobjectlist.cpp:2246, SSWater path (§1) | CONFIRMED | Tagged `<SS:Nexii>` helper, matches sswater.cpp:314-324 usage. |
| Firestorm precedent fsareasearch.cpp:2224-2227 fake-valid select nodes (§1, §4) | CONFIRMED, with nuance | The lines exist and do `mValid = true` + `mPermissions->init/initMasks`. But there the masks came from *real* sim `ObjectProperties` data gathered during the scan; Design 4 synthesizes PERM_ALL + name/desc for objects that never had a sim. Precedent is real, the design's use is a broader stretch. |
| `sendListToRegions` both overloads 5940/5949 is the funnel for the listed messages (§4) | CONFIRMED | 5940/5949; `ObjectDelete` (4472), `DeRezObject` (4431), `ObjectDuplicate` (4723/4778), `ObjectLink` (5441), `ObjectBuy` (5019), `ObjectOwner` (4971), `ObjectPermissions` (5063), `MultipleObjectUpdate` (4903), `ObjectName`/`ObjectDescription` (5255/5286/5286), `Undo`/`Redo` (8001/8019). Empty-queue bail at 6053-6057 makes "filter SS nodes out of the send queue" clean. |
| Inline `ObjectSelect` in `selectObjectOnly` (516-525); two inline `ObjectDeselect` (985, 1031) (§4) | CONFIRMED | 518-525, 985, 1031. These are hand-rolled, correctly listed as separate gates. |
| `requestObjectPropertiesFamily` (6156), `requestObjectPropertiesViaSelect` (6173-6198) (§4) | IMPRECISE (off by a few lines) | Functions at 6152 and 6173-6198; the design's §11 "6183-6197" covers the packet body. Right functions, sloppy lines. |
| `canSelectObject` (8128), `permYouOwner` false for local objects (§4) | CONFIRMED | 8128; perms read object *flags* (llviewerobject.cpp:7198-7313), unset for client-side VOs. The early-true is genuinely needed. |
| Node consumers gate on `node->mValid` (3459+) (§4) | CONFIRMED | ~30 gate sites from 3459 onward. |
| "LLManip already applied the final transform to the child VO before `sendMultipleUpdate`" (4892) (§4) | CONFIRMED, but see Fatal 2 | `packMultipleUpdate` reads `object->getPosition()` live (4930-4947); manip writes positions during drag (llmaniptranslate.cpp:793). The *capture point exists*, but the node never reaches it (see below). |
| `LLToolSelect::handleObjectSelection` (lltoolselect.cpp:82) is the click-select funnel (§4) | CONFIRMED | Called from lltoolcomp.cpp:190/291/417/631 (all build tools) and lltoolpie.cpp:373/387/400/2346 (click + right-click). One redirect here genuinely covers click-select for all composite tools and the pie menu. |
| `LLToolSelectRect::handlePick` (lltoolselectrect.cpp:75-82) redirect makes rubber-band select children individually (§4, §11) | WRONG | `handlePick` (82) only captures the pick to start the drag. Rect selection collects objects in `LLToolSelectRect::handleRectangleSelection` (llglsandbox.cpp:79) → `highlightObjectOnly` (llselectmgr.cpp:1232) and *commits* in `selectHighlightedObjects()` (llselectmgr.cpp:1356). The cited lines 75-82 contain no selection. The redirect must go into `highlightObjectOnly`/`selectHighlightedObjects`. |
| `addAsIndividual` (1106): "both `selectObjectOnly` and `addAsFamily` funnel through it" (§4) | WRONG | `addAsFamily` (1052-1101) news its own `LLSelectNode`s at 1068 and never calls `addAsIndividual`. Only `selectObjectOnly` (→506) funnels through it. The single annotation point misses every family selection — including the design's own editor "select all" (`selectObjectAndFamily(root)`). |
| Fake-valid pattern suffices for manipulators (§4: "LLManip works off the (now valid) nodes") | WRONG | Manipulators gate on the VO's own perms: `object->permMove()` (llmaniptranslate.cpp:713), `permModify() && permMove()` (llmanipscale.cpp:812/934/961/1012/1213, llmaniprotate.cpp:477/569/661/753). `LLViewerObject::permMove/permModify/permCopy/permYouOwner` read object *flags* (llviewerobject.cpp:7198-7330), not the select node. A locally-created child has all flags clear → **the manipulators refuse to drag at all**. Node annotation does not fix this; the VOs need fake flags/perms too. |
| "Selection machinery: LLSelectMgr wholesale, with one fake-valid annotation step" | WRONG (undercounted) | Needs at minimum: `addAsIndividual` annotation, `addAsFamily` annotation (or `LLSelectNode` ctor), the highlight path, VO flag/permission stamping, `highlightObjectOnly` perm-gate bypass (1244-1261 re-implement the perm checks inline, `canSelectObject`'s early-true does not reach it), and a `send_type` fix in `sendMultipleUpdate` (below). This is not "one gate". |
| CalcLOD virtualization: `updateLOD` virtual (llvovolume.h:247), `calcLOD` not (llvovolume.h:433), body at 1664 (§6) | CONFIRMED | Insert-before-`sDistanceFactor` (1755) works; near-boost ramp (1757-1765), FOV factor (1770-1774), HUD/DebugSelection branches (1778-1787) all live in the body as claimed. |
| Per-child LOD tick: `updateDistance` → `updateLOD` (lldrawable.cpp:962-966; pipeline.cpp:3544) (§1) | CONFIRMED | lldrawable.cpp:962-966 (pipeline.cpp:3544 is the avatar branch). |
| `put` at llspatialpartition.cpp:948; per-child octree entries for statics (§0, §7) | CONFIRMED | 948-972; static drawables → `gPipeline.getSpatialPartition(mVObjp)` (lldrawable.cpp:1214-1219); per-region `LLVolumePartition` at llviewerregion.cpp:729. The §0 honesty correction itself is fully verified. |
| `LLVolumePartition` 5443 / `LLVolumeBridge` 5454 (§0, §9) | CONFIRMED | llvovolume.cpp:5443/5454. |
| Edit-drag bridge claim: "root briefly goes active, linkset hops into one LLVolumeBridge" (§9) | CONFIRMED | Drag marks child moved → `LLDrawable::moveUpdatePipeline` → `makeActive()` (lldrawable.cpp:761-766) which propagates to parents (536); `getSpatialPartition` then hands the active root its own `LLVolumeBridge` (lldrawable.cpp:1252-1276). |
| `adoptViewerObject` llviewerobjectlist.cpp:2246 (§1) | CONFIRMED | Also calls `updateActive` — static objects don't join `mActiveObjects` (1820-1841, `isActive()`-gated), so §9's "no per-frame idleUpdate" claim holds. |
| `setRegion` re-anchor + xform/markMoved (§2, §5) | CONFIRMED | 7417-7451; created-list calls are `local_id > 0`-guarded no-ops for localID 0 (llviewerregion.cpp:3049-3067). |
| Free-child math `rel = global − root_global` correct across region sizes (§5) | CONFIRMED | Root at origin, identity rot/scale → parent-relative frame ≡ region frame; `LLVector3d` global minus origin is precision-safe. Only gap: hardcoded X/Y "0–256" ignores varregions (`region->getWidth()` exists; sswater already uses `getWidth()`). |
| R4 same-UUID `setVolume` re-refs cached volume, no fetch (§2) | CONFIRMED | `loadMesh` dedupe (4494-4514) + `refLOD(last_lod)` reuse (4527-4537); system-volume refcount in `notifyMeshLoaded` (4979-4993). |
| `SSWaterWorld` pattern: `clearWaterObjects` 252-267, `anyDead` 270 (§2, §8) | CONFIRMED | sswater.cpp:252-267, 270-287; signature-diff update at 163-175. |
| SSWaterWorld tick after applier at llviewerdisplay.cpp:1000 (§1) | CONFIRMED | Injection point exists exactly as cited. |
| Applier clamp 183-187; `put()` cache-and-diff 1041-1049 (§8, §5) | CONFIRMED | ssatmoenvapplier.cpp:183-187, 1041-1049; `isActive()` exists (ssatmoenvapplier.h:123). |
| `saveNotecard`/`updateExistingNotecard` (690-724), `isModified()` diffs working copy (§5) | CONFIRMED | ssatmoenvmanager.cpp:690/707/724, isModified at 58; `editable()` at ssatmoenvmanager.h:59. |
| `SS_ATMOENV_VERSION` round-trip tolerates old documents (§3) | CONFIRMED | `sd["version"]` written at ssatmoenvasset.cpp:1630, read with `sd.has("version")` fallback at 1663; per-field `has()` tolerance throughout. |
| `getIsFullPerm` 2482; `DAD_MESH` llviewerassettype.cpp:81; `SSTextureListCtrl::handleDragAndDrop` ssfloatertexturelist.cpp:175 (§2) | CONFIRMED | All exact. |
| `getBinRadius` llvovolume.cpp:4812 (§7) | CONFIRMED | |
| 404 row: `is404` 1237-1289, `notifyMeshUnavailable` 5010, `LLSculptIDSize`/`isMeshAssetUnavaliable` (§8) | CONFIRMED | `LLSculptIDSize` (llsculptidsize.h), `LLVolume::isMeshAssetUnavaliable` (indra/llmath/llvolume.h:1136 — note the stock typo is real). |
| `fsareasearch.cpp:2224-2227` (§1) | CONFIRMED | See row above. |
| `validateSelection` flushes dead selections (§10) | CONFIRMED | llselectmgr.cpp:8112. |
| "Stock Edit-menu undo cannot work (in-world undo is server-side)" (§4) | CONFIRMED | `LLSelectMgr::undo()` (7997) is send-only; no local transform revert exists. With gated sends it's a true no-op. `canUndo()` keys on fake-valid nodes, so the *menu item stays enabled* — a dead menu item the design doesn't list. |
| Mesh plumbing: no `LLVolumeImplMesh`; setVolume 1229; loadMesh 4482; notifyMeshLoaded 4963; dedupe 4494-4514; 404 proxy 1283-1289 (§0) | CONFIRMED | All verified as above; `constructUrl` 1464, caps at 4796-4799. |

## Fatal / major findings

**F1 — The editing surface is dead on arrival as specified: manipulators cannot move SS children.**
Every manipulator drag is gated on the VO's own permissions — `object->permMove()` /
`permModify()` (llmaniptranslate.cpp:713, llmaniprotate.cpp:477/569/661, llmanipscale.cpp:812/934/961/1012/1213)
— and these read object *flags* (llviewerobject.cpp:7259-7330), which are all unset for a
locally constructed VO. The design fakes permissions only on the select node. Result:
translate/rotate/scale no-op before any packet is built. The fake-valid node makes panels
*display* correctly while the manipulators do nothing. Fix is cheap (stamp
FLAGS_OBJECT_YOU_OWNER/MODIFY/COPY/MOVE or a real `mPermissions` on the VO at creation) but
the design never says it, and its own §11 diff list has no entry for it.

**F2 — The transform capture point never fires under default settings.** `sendMultipleUpdate`
(llselectmgr.cpp:4892-4896) picks `SEND_ONLY_ROOTS` when `EditLinkedParts` is off and TE
mode is off. `is_root` (llselectmgr.cpp:8217-8221) requires `!mIndividualSelection &&
isRootEdit()`, and `LLVOVolume::isRootEdit()` (llvovolume.cpp:2442) is false for any
non-avatar-parented child. So an individually-selected child — the *only* thing the
design's redirect ever selects — is never queued into `sendListToRegions`, the packet
bails at 6053, and the claimed `captureTransform` hook never runs. Meanwhile the drag
*did* move the VO locally (`setPositionParent`, llmaniptranslate.cpp:793). Result: the
mesh visually moves, the record never updates, and the whole thing snaps back at the next
re-anchor or track crossing. The design's "EditLinkedParts is irrelevant" (§4) is exactly
backwards: it decides the send type. Name/desc survive only by accident
(`selectionSetObjectName` falls back to `SEND_INDIVIDUALS` when `getRootObjectCount()==0`,
llselectmgr.cpp:5263-5272 — and silently targets the root instead if the author has
"select all" active).

**F3 — The invisible root is rubber-band selectable, and rubber-band selection is
unhooked anyway.** `LLToolSelectRect::handleRectangleSelection` (llglsandbox.cpp:234-297)
culls drawables straight out of every region partition with only pcode/attachment checks —
no INVISIBLE or `isVisible()` filter — and the root is a legitimate `PARTITION_VOLUME`
citizen at the region origin. `highlightObjectOnly` (llselectmgr.cpp:1232-1266) has no
visibility gate either. Any rubber box containing the region origin highlights the root;
`selectHighlightedObjects` then selects it (the design's `canSelectObject` early-true makes
this *more* likely, not less). Dragging that root moves the whole landscape visually with
no record to capture into — silent divergence until the next re-anchor snaps it back.
This directly falsifies "Selecting the root through the world is impossible by
construction (§4)". The same missing `highlightObjectOnly` gate means children are
unrubber-bandable whenever `SelectOwnedOnly`/`FSSelectCopyableOnly` is on (perm flags
unset, inline gates at 1244-1261).

**F4 — The single annotation point is wrongly scoped.** The design's claim that "both
`selectObjectOnly` and `addAsFamily` funnel through `addAsIndividual`" (§4, llselectmgr.cpp:1106)
is wrong: `addAsFamily` news its own nodes (1068). Consequences: the editor's own
"select all" (`selectObjectAndFamily`) and rubber-band commit (`selectHighlightedObjects`,
which copy-constructs nodes at 1385, inheriting `mValid` from highlight nodes, 7133)
produce un-annotated, invalid nodes — exactly the dead panels the fake-valid trick was
meant to prevent. The Firestorm precedent at fsareasearch.cpp:2224 is real but narrower:
it stamps *sim-verified* properties onto nodes for a one-shot return action, not
synthetic full-lifetime identities for the build floater.

**F5 — Cost/benefit has quietly inverted.** Once F1-F4 are fixed, the select-mgr surgery
is no longer "one gate + one annotation": it is `sendListToRegions` (+ send-type fix for
SS-containing selections), `selectObjectOnly` inline select, two inline deselects, two
properties-request functions, `canSelectObject`, `addAsIndividual`, `addAsFamily`, the
highlight path, VO fake-perm stamping, plus the two tool redirects — ~10 injections across
3 files, all in the most traffic-heavy class in the viewer. Against that, the compound
root's honest wins (per §0/§9, conceded in-design) are: one reconcile loop, one kill
path, one setRegion cascade. Those same wins are available to an N-root design via the
same `SSAtmoLandscapeWorld` singleton managing a list of root objects — no linkset, no
select-mgr surgery at all, and picking/selection needs *zero* gates because the objects
are simply non-pickable. The design never argues why one linkset root beats N roots under
one manager; it only argues why the *rejected octree claim* was wrong.

**F6 — R4 adoption is correct, but the reconcile signature lacks the live child list
state.** The signature (active, track index, region handle, record revision) misses
*selection state* and *in-flight child* state: a child selected during a track crossing
that keeps it (same UUID) stays selected while its record transform is overwritten
(record adoption "one frame, no tween") — the selection's node transform snapshot
(`mSavedPositionLocal`) is now stale, and the next undo/manip commit reverts to stale
state. The design only handles the replace/kill case. Undefined: what happens to an open
manipulator drag when a track crossing lands mid-drag.

## Minor findings

- §0/§7 "pipeline.cpp:3526 skips INVISIBLE drawables in the distance/LOD pass" — imprecise:
  3526 gates `setVisible` only; the invisible root still runs `updateDistance`/`updateLOD`
  every frame (negligible, but the stated reason is wrong).
- Varregions: locked X/Y "clamped 0–256" (§3, §5) hardcodes 256; the brief says the same,
  but `LLViewerRegion::getWidth()` exists and sswater already branches on it. Locked coords
  should clamp to region width, not a constant.
- `addAsIndividual` citation drift: §4 says 1106, §11 says 1114 — both are real (function /
  node creation) but pick one.
- `requestObjectPropertiesFamily` is at 6152, not 6156 (6156 is the `newMessageFast` line).
- Area search leak: `FSAreaSearch::isSearchableObject` (fsareasearch.cpp:638-662) passes any
  `mbCanSelect` volume in the region — the 64 children (and the root, if ever visible)
  appear in FS Area Search with localID 0 and property requests that never resolve (the
  gate blocks the packet, so rows hang "requested"). SSWater has the same exposure, so
  there is precedent, but 64×8 phantom rows is a different magnitude. Either gate
  `isSearchableObject` too, or set `mbCanSelect=false` (which would break picking — the
  design must pick one).
- Fake name/desc/creator in `annotateNode` will also leak into the Inspect floater and
  copy/paste paths (paste of position/TEs onto a *real* object works — that's fine — but
  paste from an SS node to a real object sends real packets from real objects; only the SS
  side is gated).
- §8's "deselectObjectAndFamily(child)" flush actually walks to the linkset root and
  deselects the whole family (`deselectObjectAndFamily` → `getRoot()`, llselectmgr.cpp:960-967) —
  harmless here, but the stated "before killing replaced children" also needs the
  annotation step for the family nodes or `deselectHighlightedObjects`-style paths will
  iterate dead VOs (stock `findNode` tolerance does cover it).
- The claimed "one reconcile loop" still needs a per-frame `anyDead` sweep over ≤64 children
  (as SSWater does) — fine, but §9's "sub-µs when clean" ignores that `anyDead` touches 65
  `LLPointer`s/frame; still negligible, just not zero.
- Editor "select all" via `selectObjectAndFamily(root)`: name/desc edit with root+children
  selected takes the `SEND_ONLY_ROOTS` branch (llselectmgr.cpp:5251) → the name edit
  captures on the root only. Undefined which record, if any, that writes.

## Silently undefined behaviour

- What sets `LLDrawable::INVISIBLE` on the root's drawable, and when. No stock path sets
  it for a pcode-volume drawable; the design must do it manually every rebuild (some
  rebuild paths clear/rewrite drawable state).
- Whether the root participates in `EditLinkedParts` toggle, "Select Only My Objects",
  RLVa `canEdit` gates (lltoolselect.cpp:91-124 runs before the design's redirect).
- TE capture on hydrate when the mesh has fewer faces than the record's TE list is defined
  ("applied up to the count the mesh actually has") but not when the mesh arrives *after*
  a TE was already applied via the build floater.
- What happens to the selection when the whole set is killed on unload while the build
  floater is open (design claims `validateSelection` handles it; plausible but untested
  for 64 simultaneous dead nodes).
- What `captureTransform` does for the root node if it ever ends up selected (F3) — the
  root carries no record.
- Free-mode children whose global position falls outside all loaded regions at re-anchor
  time (far scenery at 2048 m from a region corner).
- Duplicate asset UUIDs in one track's list: `mRecordIndex` disambiguates, but the design
  never says the editor forbids duplicate asset UUIDs, nor whether R4 adoption matches the
  first or Nth duplicate.

## Steal-worthy parts

1. **§0's two corrections.** Both verified: there is no `LLVolumeImplMesh` — mesh fetch is
   per-`LLVOVolume` sculpt-param machinery (`setVolume` llvovolume.cpp:1229 → `loadMesh`
   4482 → `notifyMeshLoaded` 4963, deduped by `mLoadingMeshes` 4494-4514) — and static
   compound roots get one octree entry *per child drawable* (`LLDrawable::getSpatialPartition`
   lldrawable.cpp:1208-1290). Any synthesis should carry both facts verbatim.
2. **Root-at-region-origin identity trick.** Identity rotation + unit scale at the region
   origin makes parent-relative == region-local, so locked records need zero re-anchoring
   math and free records are one subtraction. This is the cleanest R6 mechanism of any
   design in the set, and it is verifiable against `LLViewerObject::setRegion`'s child
   recursion (llviewerobject.cpp:7438-7442).
3. **R4 same-UUID adoption via `setVolume`.** A same-UUID `setVolume` re-refs the cached
   system volume (`mLoadingMeshes` dedupe + `refVolume` at llmeshrepository.cpp:4981) —
   no fetch, no pop. UUID-keyed reconcile with `mRecordIndex`+`mAssetID` on the child is
   the cheapest correct R4 matcher in the five designs.
4. **The honest octree ledger + per-child LOD.** Killing the compound-bbox claim and
   keeping per-child drawables in `PARTITION_VOLUME` is both correct (verified against
   `getSpatialPartition` lldrawable.cpp:1208-1290) and the right culling call for scattered
   scenery; per-child LOD then falls out for free (each child owns `mDistanceWRTCamera`).
5. **Failure-mode table shape.** Per-child 404 degradation, `anyDead()` sweep rebuild with
   refcounted volumes (transform re-apply, not re-fetch), and the "record never mutated by
   failure" rule are all stock-mechanically sound and worth copying into the synthesis.

## Verdict

**Feasibility: 4/10.** The mesh/culling/reconcile/LOD substrate is real and verified — the
design's honesty about the octree is genuinely valuable. But the editing story, which is
the design's whole justification for a linkset, rests on four unverified stock behaviours
(VO perm flags, `SEND_ONLY_ROOTS`, `addAsFamily` annotation, rubber-band highlight) that
are each individually enough to break the editing loop, and the fix for the first two adds
the same per-object surgery an N-root design would have needed anyway — plus the
select-mgr surface on top. The design's honest-ledger §9 concedes the octree win was
illusory; what remains is a reconcile loop that didn't need a linkset to exist.

Three worst problems:
1. Manipulators gate on VO flags the design never fakes — drags, scale, rotate, and
   texture drops all refuse before any capture hook runs (F1).
2. Transform/name capture rides sends that never fire for individually-selected children
   under default `EditLinkedParts=off` (F2) — the design's sole commit point is dead code.
3. Selection-surface holes: annotation misses `addAsFamily` and the highlight path; the
   invisible root is rubber-band-selectable; the claimed rect-select redirect point
   (`handlePick`) does no selecting (F3, F4).

Three best ideas:
1. The §0 corrections — no octree win for statics, no `LLVolumeImplMesh` — with the
   verified per-prim mesh-fetch story (`mLoadingMeshes` dedupe, refcounted system volumes)
   that R4 adoption rides for free.
2. Root pinned at region origin with identity frame: locked records are linkset-local
   verbatim, free records are one subtraction, re-anchor is one `setRegion` cascade
   (verified at llviewerobject.cpp:7438-7442).
3. The `SSWaterWorld`-style signature reconcile (State/anyDead/rebuild) applied to a
   record list, with dead-set rebuild as the universal failure recovery.
