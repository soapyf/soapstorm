# Review: Design 1 (native local object)

Reviewer: adversarial pass, every codebase claim re-verified by grep/read against this
worktree. Line numbers below are from this tree; where the design and the code disagree,
the code wins.

## Verification of codebase claims

| # | Claim (design) | Verdict | Correction / evidence |
|---|---|---|---|
| 1 | `getIsFullPerm()` llviewerinventory.cpp:2482, modify-ok & copy-ok & transfer-ok | CONFIRMED | 2482-2496, exactly those three `allowOperationBy` tests |
| 2 | `getProtectedAssetUUID` 2464-2480 returns null for non-fullperm | CONFIRMED | 2464-2480 (fullperm OR godlike else `LLUUID::null`) |
| 3 | `adoptViewerObject` llviewerobjectlist.cpp:2246, header note .h:71-73 | CONFIRMED | map + `mObjects.push_back` + `updateActive`, tag present |
| 4 | `SSWater : public LLVOWater`, adopted at sswater.cpp:314 | CONFIRMED | class at sswater.h:48-52 (design's "44-51" includes the tag comment — nit); adoption at 314, `gPipeline.createObject` at 324 |
| 5 | SSWaterWorld State struct style, sswater.h:109-128 | CONFIRMED | exact range |
| 6 | `anyDead` idiom sswater.cpp:270-287 | CONFIRMED | exact |
| 7 | Drive site llviewerdisplay.cpp:1000 | CONFIRMED | `SSWaterWorld::getInstance()->update()` in the tagged Atmo block |
| 8 | `clearWaterObjects` in `LLWorld::resetClass` llworld.cpp:134-139 | CONFIRMED | kill before `gObjectList.destroy()` |
| 9 | `calcLOD` llvovolume.cpp:1664, non-virtual llvovolume.h:433; `updateLOD` virtual .h:247 | CONFIRMED | all three exact |
| 10 | `distance *= sDistanceFactor;` at 1755; `forceLOD` at 1828 | CONFIRMED | exact |
| 11 | `updateLOD` refuses via `LLSculptIDSize::isUnloaded` at 1847 | CONFIRMED | 1847 |
| 12 | `setVolume` 1229 issues `gMeshRepo.loadMesh(this, params, lod, last_lod)` at 1320; is404 at 1247-1251/1283-1289 | CONFIRMED | exact. Caveat: the is404 branch only runs when `isSculpted()` is *already* true (1239) — i.e. from the second `setVolume` onward. The first call fetches unconditionally; see Major M6 |
| 13 | `LL_SCULPT_TYPE_MESH = 5` llvolume.h:194 | CONFIRMED | exact |
| 14 | `loadMesh` entry llmeshrepository.cpp:4482; pending dedup; "see what we can display while we wait" 4516-4557 | CONFIRMED / IMPRECISE | lines correct. But 4516-4557 only substitutes a *different, already-loaded LOD* and requires `vobj->getVolume()`; it does not hand out the same-LOD volume. R4 adoption is free because the loaded system volume sits in the `LLVolumeLODGroup` and adoption doesn't call `setVolume` — not because of this search |
| 15 | caps GetMesh/GetMesh2/ViewerAsset 4796-4799 | CONFIRMED | 4794-4799 |
| 16 | `notifyMeshLoaded` 4963 → waiting `LLVOVolume`s 4996-5002 | CONFIRMED | exact |
| 17 | `setNumTEs` llviewerobject.cpp:5406 "fans into `LLPrimitive::setNumTEs`" | IMPRECISE | `setNumTEs` is virtual and `LLVOVolume::setNumTEs` (llvovolume.cpp:2452) overrides; the world's call dispatches to the volume impl (which then calls the 5406 base at 2458/2481/2485). Behaviour is fine, the citation names the wrong override |
| 18 | `sendTEUpdate` 5528 / `sendShapeUpdate` 5507 / `sendMaterialUpdate` 5491 / `updateFlags` 7474 / `fetchInventoryFromServer` 3228 | CONFIRMED | all exact |
| 19 | `setRegion` llviewerobject.cpp:7417; created-list no-ops on localID 0 (llviewerregion.cpp:3061-3067) | CONFIRMED | both exact |
| 20 | perm flag reads permModify/permCopy/permMove at 7259/7283/7307 | CONFIRMED | exact |
| 21 | `sendSelect` → "ObjectSelect" at 5524; `sendMultipleUpdate` 4892 | CONFIRMED | both exact (5080 is a commented-out stub) |
| 22 | **"All selection sends funnel through `sendListToRegions` (5940-6145)… one gate covers every Object\* message in the viewer"** | **WRONG** | `sendListToRegions` exists as described and the empty-queue bail (6053-6057) is real, but llselectmgr.cpp itself contains **four direct sends that bypass it**: `selectObjectOnly`'s unconditional ObjectSelect at **518-525** ("Always send to simulator"), the ObjectDeselect loop in `deselectObjectAndFamily` at **985-1011** (fires when `send_to_sim`, whose default is `true`, llselectmgr.h:634), `deselectObjectOnly` at **1031-1037** (same default), and `sendGodlikeRequest`'s direct send at **3161-3163**. The click-select path is `LLToolSelect::handleObjectSelection` (lltoolselect.cpp:82, used by the build tools via lltoolcomp.cpp:190/291/417/631) which calls `selectObjectOnly` (198/223) → the 518 direct send. See Major M1 |
| 23 | `selectionSetObjectName` 5245-5274 / Description 5276 via sendListToRegions | CONFIRMED | sends at 5255/5267/5286/5298 |
| 24 | `selectDelete` (4356) → `confirmDelete` (4414) → DeRezObject (4431) | IMPRECISE | `confirmDelete` 4414 and its `sendListToRegions("DeRezObject", …)` at 4431 are exact; `selectDelete` *starts* at 4304 — 4356 is the `confirmDelete` functor bind inside it |
| 25 | `requestObjectPropertiesFamily` 6152, called from selection at 1216/1397 | CONFIRMED | exact |
| 26 | Node creation sites: addAsFamily 1068, addAsIndividual 1114, hover 1211 | CONFIRMED | all three exact (`new LLSelectNode`) |
| 27 | `llpanelpermissions.cpp:628/635` reads `nodep->mName`/`mDescription` | CONFIRMED | exact |
| 28 | Manip sends: llmaniptranslate.cpp:1079 "and friends" | CONFIRMED | translate 1079; rotate 488; scale 414/1189 — all `sendMultipleUpdate` (→ sendListToRegions, gated) |
| 29 | `send_ObjectGrab_message` lltoolgrab.cpp:1185 | CONFIRMED | exact — but the design misses the **ObjectGrabUpdate** direct sends at lltoolgrab.cpp:731 and 900 (grab-drag update). See M4 |
| 30 | Drag-and-drop: `dropMaterial` 1238, `dropMesh` 1568, `dropTexture` 1592; `setTEGLTFMaterialOverride` client-side before `sendTEUpdate` (tail at 1235) | CONFIRMED | all exact; note 1235 is the tail of `dropTextureAllFaces` — the one-face tail is 1824. Nit |
| 31 | `derez_objects` llviewermenu.cpp:6324 | CONFIRMED | collection point as described |
| 32 | `LLWaterPartition` `mInfiniteFarClip = true` llvowater.cpp:295-301 | CONFIRMED | set at 298 |
| 33 | `LLOctreeCullNoFarClip` branch llspatialpartition.cpp:1469-1473 | CONFIRMED | `mInfiniteFarClip || (!sUseFarClip && !gCubeSnapshot)` at 1469 |
| 34 | Projection far override llviewercamera.cpp:301-306 + 352-356 | CONFIRMED | both sites exact (`z_far = MAX_FAR_CLIP` at 355) |
| 35 | Generic `NUM_PARTITIONS` enumeration (render cull 2692, restoreGL 1709) | CONFIRMED | loops exist at 1709/1811/2318/2583/2619/2692 — but see F1: the **pick** path is not generic |
| 36 | `lineSegmentIntersectWorldGeometry`'s `world_partitions` 7507-7512 (shoulder cam); "the 7352 collision list" | CONFIRMED / **WRONG attribution** | 7507-7512 is exactly as described. But pipeline.cpp:7350-7358 is **not a collision list** — it is the filter inside `LLPipeline::lineSegmentIntersectInWorld` (7323), the **primary world raycast** used by every click-pick (`pickAsync` → llviewerwindow.cpp:5750). Its explicit list (VOLUME/BRIDGE/AVATAR/CONTROL_AV/TERRAIN/TREE/GRASS) excludes a new `PARTITION_LANDSCAPE`, and pipeline.cpp is absent from the §11 diff list. See F1 |
| 37 | `PARTITION_VOLUME` at llviewerregion.h:99; insert after it | CONFIRMED | enum position exact; `initPartitions` push_backs are at 722-735, design says 716 — nit |
| 38 | `processUpdateCore` sizes TEs like the sim | CONFIRMED | llviewerobjectlist.cpp:305 |
| 39 | `adoptParsedAsset`/`applyNotecardText` replace `mWorking` (ssatmoenvmanager.cpp:932-944); `unload()` 915 | CONFIRMED | 932/872/915 |
| 40 | put() cache-and-diff precedent ssatmoenvapplier.cpp:545-553 | CONFIRMED | lambda defined at 545 |
| 41 | Applier computes `blend.mPrimaryTrack` per frame (176-187), "stores it in `mPrimaryTrack` and exposes `primaryTrackIndex()`" | IMPRECISE | compute at 176-183 confirmed; **no `mPrimaryTrack` member exists** — it is a local `track_index` (183). The addition is acknowledged in §1.2 but the prose states it in the present tense |
| 42 | Applier `isActive()` published | CONFIRMED | ssatmoenvapplier.h:123 |
| 43 | `SS_ATMOENV_MAX_TRACKS = 8` | CONFIRMED | ssatmoenvasset.h:44 |
| 44 | `hasAsset()` / `editable()` on the manager | CONFIRMED | ssatmoenvmanager.h:46/59 |
| 45 | `SSTextureListCtrl::handleDragAndDrop` ssfloatertexturelist.cpp:175 | CONFIRMED | exact |
| 46 | GenericAlert rejection pattern ssfloateratmoenv.cpp:633 | CONFIRMED | exact |
| 47 | Upload preview builds own `LLVertexBuffer`s llmodelpreview.cpp:3911-3922 | CONFIRMED | exact |
| 48 | Celestial billboards lldrawpoolwlsky.cpp:766-824 | CONFIRMED | includes the existing `<SS:Nexii>` SS disc block at 788-810 |
| 49 | `LLStaticViewerObject` llviewerobject.h:1186; sky via `createObjectViewer` llsky.cpp:204 | CONFIRMED | both exact |
| 50 | `DAD_MESH` maps AT_MESH llviewerassettype.cpp:81 | CONFIRMED | exact |
| 51 | Seeded node fields exist: `mValid`, `mName`, `mDescription`, `mPermissions`, `mCreationDate`, `saveTextures` | CONFIRMED | llselectmgr.h:229/235/236/230/255/211 |
| 52 | Permission seam: `canSelectObject` reads flag bits | CONFIRMED | llselectmgr.cpp:8128-8176 (`permYouOwner`/`permMove`/`permCopy`/`isPermanentEnforced`) |
| 53 | **"The stock viewer has no object-edit undo… no ObjectUndo message in this codebase"** | **WRONG** | `LLSelectMgr::undo/redo` send **"Undo"/"Redo"** messages (llselectmgr.cpp:8001/8019) and Edit→Undo is live for selected objects. The all-local gate bails the send (empty queue), so Ctrl+Z silently no-ops on a landscape object — a behaviour change vs stock (where Undo reverts to server state) that the design doesn't own |
| 54 | `mRegionWater` held as `LLPointer` surviving killObject for the dead sweep | CONFIRMED | sswater.cpp:260-286 pattern |

Hit rate: ~50 claims checked, 44 confirmed at the cited line, 4 imprecise, 2 materially
wrong (22, 53) — and the two wrong ones sit on the design's load-bearing wall (§4 gating,
§4 undo story).

## Fatal / major findings

### F1 (fatal as written): the object cannot be clicked — the pick path is an explicit partition list the design never touches

`LLPipeline::lineSegmentIntersectInWorld` (pipeline.cpp:7323) enumerates partitions
explicitly at 7350-7358 (`PARTITION_VOLUME, BRIDGE, AVATAR, CONTROL_AV, TERRAIN, TREE,
GRASS`). A new `PARTITION_LANDSCAPE` is skipped there. `pickAsync` reaches this function
from llviewerwindow.cpp:5750, and `handleObjectSelection` (lltoolselect.cpp:82) is the
shared click-select for the build tools, Inspect and the pie tool. Consequences with the
design's §11 diff list as written:

- "select in-world (click)" (§4's opening move) never selects the mountain;
- face-targeted texture/material drops, grab, Inspect-by-click, face picking — all dead
  (they all consume the same pick);
- the only working edit entry would be the Landscape tab's "select-in-world" button,
  which calls `selectObjectAndFamily` directly and bypasses the pick — not what §4
  promises.

The design actually *saw* this list and misidentified it: §7 calls the 7352 site "the
collision list" and files it under "the shoulder cam can clip through scenery —
deliberately not fixed." That attribution is wrong (the shoulder-cam list is
`lineSegmentIntersectWorldGeometry`'s `world_partitions` at 7507-7512, which the design
cites correctly), and the misreading flips a mandatory one-line injection into an
accepted cosmetic limitation. Fix is cheap — add `PARTITION_LANDSCAPE` to the 7350-7358
filter and a pipeline.cpp row to §11 — but a design whose editing UX silently depends on
an injection it explicitly declines to make is not implementable as specified.

### M1 (major): the "one gate covers every Object\* message" claim is false inside llselectmgr.cpp itself

Verified bypasses (all in the design's own primary file):

1. **`selectObjectOnly`** — unconditional direct ObjectSelect at 518-525, reached on
   every click-select (lltoolselect.cpp:198/223). First click on the landscape object
   sends localID-0 traffic on the hottest path in the editor.
2. **`deselectObjectAndFamily`** — direct ObjectDeselect loop at 985-1011; `send_to_sim`
   defaults to `true` (llselectmgr.h:634), so every click-away / tool-switch deselect
   leaks. (The design's own "deselect before kill" uses `(obj, false)` and is safe.)
3. **`deselectObjectOnly`** — direct ObjectDeselect at 1031-1037, same default.
4. **`sendGodlikeRequest`** — direct send at 3161-3163 (secondary).

The consequence is not a crash — the sim ignores localID 0 — but the design's thesis is
"a selection that is entirely local builds an empty queue… one gate covers every
Object\* message," and its Known-risks section claims only *new upstream* sends can
bypass. Three existing sends in this very file already do. Three more tagged gates
(518 block; `send_to_sim` early-out in the two deselect paths) close it; the diff list
must say so.

### M2 (major): the Firestorm leak inventory is understated — at least six direct senders beyond FSAreaSearch

The design's Known risks names FSAreaSearch and "radar-style listings." Verified direct
sends that a selected local object would receive or that target selected objects:

- `fscommon.cpp:278` (ObjectPermissions) and `:302` (ObjectFlagUpdate) — the FS Build
  Prefs apply path; runs on objects the user is editing.
- `fslslbridge.cpp:1197` (MultipleObjectUpdate) — the LSL bridge's object commands.
- `fsfloaterimport.cpp:845/860/873/887/953/1155` — FS Importer sends ObjectClickAction,
  ObjectName, ObjectDescription, ObjectPermissions, ObjectSaleInfo,
  MultipleObjectUpdate directly.
- `chatbar_as_cmdline.cpp:471/277/488` — cmdline `//name` / derez equivalents.
- `animationexplorer.cpp:408` and `fsareasearch.cpp:804` — direct ObjectSelect/DeSelect.

The stock-file gates the design lists (`sendTEUpdate`, `updateFlags`, etc.) do not cover
any of these — each builds its own message. Either these sites get local-content
guards (stock-diff list grows by 4-5 files, against the design's minimalism claim) or
the design must adopt the noisier-but-honest posture: "direct senders produce localID-0
traffic; the sim rejects it; we accept it for Firestorm features we don't own" — stated
per-site, not as a two-line risk note.

### M3 (major): the signature/adoption feedback loop makes adoption fire on every drag frame

`mRecordHash` folds "uuid, mode, position, rotation, scale, lod pin, enabled, face
hashes…" (§1.2 step 2). `writeBack` (§5) writes exactly those fields into the record
when the object moves. Therefore, during any manip drag: frame N writeBack dirties the
record → frame N+1 signature differs → `adoptRecords` runs → "apply the new record's
transform, scale, rotation, TEs, LOD pin" to every paired object, including the one
being dragged, with a record that is one frame stale. Per-frame order (§1.2: signature →
adoption → re-anchor → sweep → writeBack) then has writeBack re-serialise the rolled-back
position, so the manip's delta for that frame can be lost on mouse-up.

The §5 conflict rule ("the live object wins for any field it was edited in this pass")
is asserted, not mechanised: nothing in the signature distinguishes "record changed by
my own writeBack" from "record changed by revert/tab/load." The design needs one of:
hash the record side excluding writeBack-authored fields (i.e. signature tracks only
external changes), suppress adoption for selected/being-manipulated objects, or
adoption applies only when the live value differs from the record by more than epsilon
plus an active-edit check. As written, drag behaviour is emergent and unanalysed.

### M4 (major): grab path only half-gated

The design gates `send_ObjectGrab_message` (lltoolgrab.cpp:1185, ObjectGrab) but grab
dragging also streams **ObjectGrabUpdate** from lltoolgrab.cpp:731 and 900, keyed on the
object UUID. Ctrl-drag on the landscape object leaks. One more gate in lltoolgrab.cpp
(both sites) or an explicit statement that grab is refused earlier (e.g. hover tool
check) — currently absent.

### M5 (major): the schema's own worst case does not fit the notecard

The env asset is a real `AT_NOTECARD` (ssatmoenvmanager.cpp:754/116) with the standard
64 KiB asset cap. The design permits 8 tracks × 64 records (§3, §9) of pretty-XML maps
carrying ~15 fields plus a per-face array each; a fully-loaded face set is easily
300-600 bytes per record — the design's own worst case is 100-300 KB, several times the
cap, and the failure would surface at save time, far from authoring time.
`SS_ATMOENV_MAX_LANDSCAPE = 64` needs a byte-budget rationale (or a global cap, or a
save-time size check with the tab surfacing "won't fit"), none of which exists.

### M6 (minor-major): the 404 story only works from the second `setVolume` onward

The is404 branch (llvovolume.cpp:1239-1251) requires `isSculpted()` to already be true;
the first `setVolume` on a fresh object skips it and issues `loadMesh` unconditionally
(1320). For a rezzed mesh, subsequent sim updates re-trigger `setVolume`; the landscape
object's only re-trigger is `updateLOD`'s setVolume on LOD change (plus the design's own
adoption re-applications, which deliberately avoid `setVolume`). The world's "detect a
null sculpt id on a hydrated object" hook therefore needs either an LOD-change push or
hooking `notifyMeshUnavailable` (llmeshrepository.cpp:5010, the actual "asset gone"
callback the design never mentions). The claimed failure mode (§8, row 2) is
under-specified at exactly the point where it is the main user-visible failure.

## Minor findings

- `selectDelete` cited at 4356; the function starts at 4304 (4356 is the functor bind).
- `initPartitions` cited at 716; the partition push_backs are 722-735.
- `setNumTEs` virtual dispatch: the world will hit `LLVOVolume::setNumTEs` (2452), not
  the 5406 base the design cites (harmless, but the design's parenthetical is wrong).
- `mPrimaryTrack`/`primaryTrackIndex()` do not exist yet; §1.2's "it stores it" reads as
  existing behaviour.
- The R4 "loaded-mesh short-circuit" (4516-4557) mis-credits the mechanism (see claim
  14); the honest mechanism is "the LOD group already holds the loaded system volume and
  adoption doesn't call setVolume," which is actually stronger.
- sswater.h class range is 48-52, not 44-51; the 1235 sendTEUpdate tail belongs to
  `dropTextureAllFaces` (one-face tail is 1824).
- Ctrl+Z / Redo on a landscape selection is a silent no-op (gate bails the Undo send) —
  a semantics change vs stock that §4's undo paragraph should own instead of claiming
  no undo exists (claim 53).
- §6 "the build floater's position fields show region-local coordinates already" is true
  for objects parented to the agent region, but the free-mode record can park an object
  outside the agent region (global position far away) — the floater will then display
  clamped/odd values; no handling stated.
- Silhouette generation (`updateSelectionSilhouette`, llselectmgr.cpp:6790) runs on the
  selection; a 2048 m, high-poly landscape mesh makes selection outline cost spike where
  a rezzed mesh of the same size would be LOD-down. Not analysed in §9.
- The "Known risks" claim that un-gated probes produce "noisy (server rejects localID 0),
  not silent" failures is right for ObjectSelect-class messages but unverified for
  DeRezObject (fscommon/cmdline derez of a landscape object would be a *derez request*
  the sim rejects — same noise class, but worth one sentence).

## Silently undefined behaviour

1. **Adoption vs. dead instances.** Update order is signature → adoption → re-anchor →
   dead sweep. A teleport that also flips the primary track leaves killed-but-not-yet-
   swept objects in the live set; `adoptRecords` pairs by UUID against a list containing
   dead `LLPointer`s. Whether adoption skips `isDead()` is unstated.
2. **General tab permission checkboxes.** Seeded masks make every checkbox render
   enabled; `selectionSetObjectPermissions` sends are gated (empty queue), so toggles do
   nothing and silently revert on refresh. Undocumented.
3. **Contents tab.** Inventory fetch is gated so the list is empty — but the New Script
   / buttons' enable state and behaviour on a local object are unspecified.
4. **Face panel Media / PBR surface sections** beyond the persisted subset: writes land
   client-side (gated sends) and are then discarded by the next adoption. The §3 cut
   covers the schema but not the live editor surface's behaviour.
5. **"this pass"** in the conflict rule (§5) is undefined — which fields count as
   "edited in this pass," and how the world knows, given no event hooks exist.
6. **Clamping authority** for region-locked X/Y ("clamped 0..width"): enforced in
   `fromLLSD`, at placement, or in the editor? VarRegion width (>256) vs the schema
   comment "X/Y 0–256" in the brief is also unresolved.
7. **What "Take Copy" offers** for a selection containing only locals (derez filter
   removes locals — so Take Copy of a mixed selection silently copies the rest; of a
   pure local selection it's a no-op with no message). Unstated.
8. **Sun shadows on a 2048 m object**: shadow camera far plane will hard-clip the
   mountain's shadow contribution; §7 covers main-view culling only. Probably
   acceptable, but it is a visible artifact class the design doesn't name.

## Steal-worthy parts

- **Pcode-sharing adoption as a general mechanism** — `LL_PCODE_VOLUME` +
  `adoptViewerObject` + `gPipeline.createObject` (sswater.cpp:314/324 generalized):
  the entire mesh pipeline (fetch, decode, LOD group, per-face TEs, pools, octree) with
  zero bespoke geometry. The strongest idea in any of these designs; verified end to
  end here.
- **`virtual F32 ssLODDistanceScale() { return 1.f; }` + one multiply at
  llvovolume.cpp:1755** — the surgical R3. One word makes `calcLOD` virtual; the hook
  keeps the ~160-line LOD body (rigged branch, near-boost, FOV zoom, debug overlay)
  single-sourced; `mLODDistance` keeps true distance so `DebugObjectLODs` stays honest.
  Steal verbatim for any design that wins.
- **Reconcile loop instead of commit events** — the put() cache-and-diff
  (ssatmoenvapplier.cpp:545) generalized into a two-directional funnel: every editor
  path (manips, face panel, drops) converges because the live object is the only edit
  surface and diffs write the record. Kills the entire class of "which hook did I miss"
  bugs, provided M3's feedback loop is fixed by excluding writeBack-authored fields
  from the adoption trigger.
- **SSWaterWorld-style signature diffing** for record-side adoption (load, revert, track
  cross, tab edit all through one gate), with **UUID-pairing for R4** so re-download and
  rebuild pops vanish; the "instant cut at track boundaries" reading of the
  can't-interpolate idiom is clean.
- **Permission seam via construction-time flag bits** — seed `mFlags` so
  `canSelectObject` (8128), `selectDelete` and the General tab read coherent perms with
  zero new UI; R2 falls out of seeding `LLSelectNode::mPermissions` (creator/last-owner
  render at llpanelpermissions.cpp:628/635 with no code of their own).
- **Node seeding recipe** — the three exact `new LLSelectNode` sites (1068/1114/1211)
  plus `mValid=true`, name/desc, `mCreationDate`, `saveTextures`: a complete, verified
  checklist for making a server-less object look native to every panel.
- **Second partition reusing `RENDER_TYPE_VOLUME`** with `mInfiniteFarClip` (the
  LLWaterPartition/LLVoidWaterPartition two-partitions-one-render-type precedent) — no
  new pool, no new render type, generic pass pickup.
- **`getProtectedAssetUUID` doubling as the fetch gate** (llviewerinventory.cpp:2464-
  2480): the fullperm rejection and the "could never have fetched" property are the same
  predicate — tidy, and it makes the non-fullperm failure mode provably unreachable.
- **The Known-risks honesty section** — naming the gate list's decay mode (upstream send
  paths bypassing the funnel) is the right habit even though this review found the list
  incomplete on day zero.

## Verdict

**Feasibility: 6.5/10.** The core mechanism — a directly-constructed `LLVOVolume`
subclass adopted into full object-list citizenship — is sound, precedented
(SSWater/gPipeline/adoptViewerObject all verified), and the LOD hook and reconcile loop
are genuinely elegant. But the design ships three broken load-bearing pieces: the object
is unclickable as diffed (F1), the central "one gate" claim is false in three places in
its own primary file plus undisclosed Firestorm senders (M1/M2/M4), and the
signature/writeBack loop makes adoption fire every drag frame with unanalysed results
(M3). All are fixable with roughly four extra tagged injections and one hash-exclusion
rule — none invalidates the thesis, which is why this stays above passing — but as
written the §11 diff list is incomplete and the §4 editing story does not function.

**Three worst problems:**
1. F1 — pick path excludes `PARTITION_LANDSCAPE` and pipeline.cpp is missing from the
   diff list; the design misread the 7352 list as a collision list when it is the
   world raycast.
2. M1/M2 — the "one gate covers every Object\* message" claim; direct sends at
   llselectmgr.cpp:518/985/1031 plus six Firestorm files leak localID-0 traffic on the
   primary select/deselect path.
3. M3 — record-hash signature folds fields writeBack mutates, so adoption re-applies
   stale records mid-drag every frame; the conflict rule has no mechanism.

**Three best ideas:**
1. The `ssLODDistanceScale()` one-multiply hook — surgical, shared LOD physics, debug
   overlay stays truthful.
2. The reconcile funnel (generalized put()) — one convergence point for every editor
   path, no commit step to miss.
3. Pcode-shared adoption with UUID-paired record adoption — full stock mesh pipeline
   and pop-free track crossings for one flag and a handful of gates.
