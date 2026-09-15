# Adversarial review: "Implement Atmo Magic landscape scenery" stock-file surgery

Reviewed commit: `47a929a216` (HEAD of `design/atmo-landscape`), worktree `D:\soapstorm\design-worktree`.
Verification method: read the actual post-merge code at the cited lines; no builds, no edits, no git writes.
Baseline: the gate-site list enumerated in `review_3_author_real_render_local.md` (rows 14-16, 32-35) and the
"what does the select/grab/derez funnel actually contain" questions in the brief.

---

## Fatal

### F-1. The SSLandscapePartition never receives the landscape object; the whole partition gambit is inert, and the headline "renders past draw distance" cannot happen by any code path here
- The object is a root, non-attachment `LLVOVolume` (ssatmolandscapeobject.h:51, ctor load `LL_PCODE_VOLUME`).
  `LLDrawable::getSpatialPartition()` therefore takes the *bridge* branch (`!getVOVolume()` false, `isStatic()` false as soon as the drawable activates, `isRoot()` true) and creates an
  **LLVolumeBridge** for it — lldrawable.cpp:1270-1274 — which is `put` into the region's **PARTITION_VOLUME**
  (lldrawable.cpp:1315-1321). The drawable's octree home is the bridge, never `SSLandscapePartition`.
- Only water-like objects avoid the bridge — they are NOT `LLVOVolume` subclasses (the `!getVOVolume()` branch,
  lldrawable.cpp:1214-1218), which is exactly why the SSWater pattern works. The landscape object is a volume,
  so it gets volume treatment.
- Consequences: (a) `SSLandscapePartition` (llspatialpartition.h:712-730, pushed at llviewerregion.cpp:731) is an
  empty octree; the pick whitelist addition (pipeline.cpp:7371) and the rebuildDrawInfo addition (pipeline.cpp:12845)
  are dead code (the object is already pickable/highlightable through the pre-existing VOLUME entries);
  (b) the lone functional dividend of the partition — `mInfiniteFarClip=true` — never applies, **and even if the
  object did sit in that partition it would not help**: the base cull in this branch already uses the NoFarClip
  frustum tests for every partition (llspatialpartition.cpp:1081-1101, upstream "x-ws-merge" stock), and the
  AABB-vs-sphere check still rejects everything beyond ~`mFrustumCornerDist` ≈ 1.2×`mFarPlane`
  (llvieweroctree.cpp:1384-1387; camera far = DrawDistance, llagentcamera.cpp:226, llviewerdisplay.cpp:270).
  `mInfiniteFarClip` (read only at llspatialpartition.cpp:1469) merely swaps LLOctreeCull for the
  line-identical LLOctreeCullNoFarClip.
- Net effect: scenery renders, culls, shadows and picks exactly like an ordinary region prim, clipped at the
  user's DrawDistance. The design brief's "renders out to region far distances the way the sky does" fails by
  construction; the LOD stretch then only smooths the pop *within* draw distance.
- One-line fix: either (a) accept ordinary-volume far clipping, drop the partition class, the two pipeline
  whitelist lines and the `getPartitionType()` override, and let the object ride its volume bridge; or (b) for
  real sky-like far rendering, make `SSAtmoLandscapeObject` a plain `LLViewerObject` (water pattern, own
  createDrawable) OR give `LLVolumeBridge` the far clip (bridge ctor, llvovolume.cpp:5456) — and separately
  loosen the camera far for the Atmo frame, since the flag alone buys nothing in this tree.

---

## Major

### M-1. Select-node seeding is keyed on the wrong UUID; the editor's metadata/permission seeding never fires
- `ss_seed_local_select_node` looks the record up with `nodep->getObject()->getID()` (ssatmolandscape.cpp:85),
  but `ss_landscape_record_for_mesh` matches `record.mMeshId == mesh_id` (ssatmolandscape.cpp:70), and the object's
  `mID` is a freshly generated UUID (createObject, ssatmolandscape.cpp:239) that never equals the mesh-asset id
  the records are keyed by (`record.mMeshId = item->getAssetUUID()`, ssatmolandscape.cpp:416).
- Effect: every seeded node keeps its default blank state (mValid=false, empty name/perms, zeroed masks), so the
  General tab, Inspect and the permission-derived menu enablement show "(no object name)"/no-perms forever —
  the exact failure the seeding was written to fix. The planting of the two call sites in
  llselectmgr.cpp:1088-1090 / 1138-1140 is correct; the helper's key is wrong.
- One-line fix: key on the mesh id — `if (const SSAtmoLandscapeObject* o = dynamic_cast<const SSAtmoLandscapeObject*>(nodep->getObject().get())) ss_landscape_record_for_mesh(o->meshId())`.

### M-2. Capture vs. applyFaces race wipes authored face state before the mesh arrives (data loss into the asset)
- `update()` calls `captureAll` on a 0.25 s timer unconditionally (ssatmolandscape.cpp:176-180). Until the mesh
  fetch lands, `getNumFaces()` == 0 (ssatmolandscapeobject.cpp:131), so `captureToRecord` builds an *empty* face
  list (ssatmolandscapeobject.cpp:222-240), sees `faces.size() != mAuthored.mFaces.size()`, and overwrites
  `record.mFaces` with the empty list (ssatmolandscapeobject.cpp:242-260) — on a cold mesh cache this happens
  on every recreate (fresh record add, region crossing rebuild, track-crossing reconcile) and the wiped state
  then flows into the working asset and the next save.
- One-line fix: in `captureToRecord`, bail while the object has no applied geometry — e.g. skip when
  `mAppliedFaces < 0` (or `getNumFaces() == 0`) — mirroring the `applyFaces()` early-out.

### M-3. llviewermenu derez/Take/Return paths are ungated and the Delete path never removes local objects client-side
- `get_derezzable_objects` (llviewermenu.cpp:6217-6318) walks the full selection with no `ssIsLocalContent()`
  check; the seeded full-perm node state makes the perm test pass, so a local scenery object is included and
  `derez_objects` sends **DeRezObject with its (fake) LocalID** and the region host (llviewermenu.cpp:6380-6404)
  for Take/Return-to-owner/Acquire. Junk packet to the sim (unknown id), and Take shows as "success" while
  nothing was taken.
- The Trash/Delete path instead sends nothing at all: `confirmDelete` → DeRezObject through the funnel
  (llselectmgr.cpp:4458-4463) and `selectForceDelete` → "ObjectDelete" (llselectmgr.cpp:4496-4505) both filter
  local roots out; there is no client-side kill in derez (the sim's KILL packet drives removal for real
  objects), so **pressing Delete on a scenery object does absolutely nothing** — the object stays in place,
  still selected. There is no "client-side kill then re-hydrate" anywhere; that behavior does not exist.
- One-line fix: skip local-content roots in `get_derezzable_objects` (so Take/RTO never include them) AND route
  the confirmDelete/forceDelete path for all-local selections into `removeRecord`/`clearLandscapeObjects`
  (or document that pie-menu Delete is intentionally inert and disable it via the node-perm gate).

### M-4. The partition fix "forgot" that pick/cull/render keying is fine but shadow-probe and LOD slop ride the wrong partition
- Covered by F-1 (the object hangs off a VOLUME bridge): reflection-probe auto-registration is
  `PARTITION_VOLUME`-only (llreflectionmapmanager.cpp:669) — scenery gets no probes either way — and the
  bridge's `mInfiniteFarClip` is false, so even the object's own far clip is stock. Nothing in
  `SSLandscapePartition` (geometry managers, LOD period, depth mask) needs extra wiring — the class is a
  faithful `LLVolumePartition` clone — but none of it is reachable. Flagged separately from F-1 because it is
  a "the list itself missed" item: **the volume pipeline keys on partition membership only through
  `mPartitionType`/`mDrawableType`, and the reachable partition for this object has `mPartitionType =
  PARTITION_VOLUME`** (lldrawable.cpp:1309), so every `mPartitionType`-based behavior (alpha pool particle
  checks are the only ones) treats scenery as stock volume — which is correct only insofar as the far clip is
  also stock.

---

## Minor

- **ObjectSpinUpdate is sent for local objects** — lltoolgrab.cpp:583-611: `mSpinGrabbing` path composes a spin
  and sends with the object's random UUID, no gate (the physical spin path on a local object). Junk to the sim
  (unknown UUID), but it is a "server send touching a local object" the contract promised to suppress.
  Fix: guard the send with `!objectp->ssIsLocalContent()`.
- **ObjectDeGrab on grab release is ungated** — lltoolgrab.cpp:1137-1145 (`stopGrab` sends ObjectDeGrab for
  GRAB_ACTIVE_CENTER/GRAB_NONPHYSICAL/GRAB_LOCKED); the two grab-update sites are gated (732-756, 905-938) but
  the release degrab is not. Fix: same guard.
- **Touch sends are ungated** — `handle_object_touch` (llviewermenu.cpp:3702-3703) and the touch-by-UUID commands
  (chatbar_as_cmdline.cpp:977/996; fsareasearch/llagentlistener) send ObjectGrab/ObjectDeGrab for any
  `gObjectList::findObject` hit, including a local scenery mesh. Benign server-side, violates the contract. Fix:
  guard `send_ObjectGrab_message`/`send_ObjectDeGrab_message` (lltoolgrab.cpp:1197/1233) on
  `ssIsLocalContent()`.
- **push_some is unfiltered** — in llselectmgr.cpp the funnel gate lives only in `push_all` (6020-6031);
  `push_some` (6032-6048) does not filter, so ObjectBuy (6057-6060, SEND_ONLY_ROOTS) and Undo/Redo
  (8045/8063, SEND_CHILDREN_FIRST) can carry local-object LocalIDs through `sendListToRegions`. Fix: add the
  same `!ssIsLocalContent()` check to `push_some::apply` so every send_type is covered.
- **fsfloaterimport metadata sends are ungated** — setPrimPosition is gated (fsfloaterimport.cpp:1156) but
  ObjectName/ObjectDescription/ObjectPermissions at fsfloaterimport.cpp:860-947 have no gate. Only reachable for
  server objects the importer itself created (defensive gap, not a live hole). Fix: same top-of-function guard.
- **Landscape tab content is clipped at the floater's minimum height** — the panel is 580×1180 tall with the
  button row at top 1040-1091 (panel_ss_atmo_env_landscape.xml:6-11, 50-94), but the tab container is only
  ~725 px at the floater's min_height=897 (floater_ss_atmo_env.xml:8, 660-744); the Delete/Lock/Select buttons
  and hint are unreachable until the floater is manually resized to ~1263 px. Fix: shrink the panel's list and
  button offsets to fit the container (or raise the floater min_height).
- **llspatialpartition.h now has an inline ctor referencing LLVOVolume without including llvovolume.h** —
  `SSLandscapePartition`'s inline ctor uses `LLVOVolume::VERTEX_DATA_MASK` (llspatialpartition.h:716) while the
  header's include set (llspatialpartition.h:32-50) only pulls llvoavatar.h; it compiles today by transitive
  luck in the PCH/unity build. Fix: `#include "llvovolume.h"` in llspatialpartition.h.
- **`mInfiniteFarClip` flag is dead weight in this branch** even for future placement — base cull already does
  no-far-plane tests (see F-1); if the far rendering is ever wanted, size the camera far (llviewerdisplay
  display_update_camera) rather than relying on the flag.
- **b) empty-packet sends: none** — `sendListToRegions` returns at llselectmgr.cpp:6084 when the filtered
  node queue is empty, before the message header is created; per-region headers are never sent for all-local
  selections, and the `deselectObjectAndFamily` loop (988-1021) similarly never opens a message when every
  object is local (start_new_message stays true).
- **ObjectGrabUpdate validation races** — cosmetic debug data updated only inside the gate in
  handleHoverNonPhysical (905-938), so `mLast*` stays stale after a local drag; first remote grab after a local
  drag re-sends with a "changed" delta. Harmless.

---

## Verified-correct highlights

- **Enum/vector alignment is exact** — `PARTITION_LANDSCAPE` inserted after `PARTITION_VOLUME`
  (llviewerregion.h:100-103) and the `push_back(new SSLandscapePartition(this))` sits immediately after
  `LLVolumePartition` (llviewerregion.cpp:729-732); every other partition resolves to the correct class, and the
  `type < PARTITION_VO_CACHE` guard in `getSpatialPartition` (llviewerregion.cpp:3923) is still index-correct.
- **calcLOD hook is sound and correctly single-sited** — the `ssLODDistanceScale()` fold-in is the only distance
  multiply in `calcLOD` (llvovolume.cpp:1755-1757); all distance consumers in the body — the near-ramp boost
  (:1759-1767), F_PI/3 (:1770), `computeLODDetail` (:1792), `mLODAdjustedDistance` (:1778) — flow through the
  scaled value; `mLODDistance` (:1733) and `mAppAngle` (:1820) stay unscaled but are only consumed by debug text
  and `LLVOTree`, so nothing but the texture-LOD softness (already accepted in the design review) is affected.
  `1.f` default makes the stock path bit-identical, and mak-ing `calcLOD` virtual touches no vtable storage.
- **Header surgery is ABI-clean** — `mIsLocalContent` is a plain bool with in-class init (llviewerobject.h:831),
  no virtuals added; only per-build layout change.
- **Every listed llselectmgr gate exists and is placed correctly**: selectObjectOnly ObjectSelect (521-531),
  both deselect paths (988-1021, 1041-1056), funnel push_all (6022-6028), both property-request functions
  (6184-6189, 6212-6217), sendSelectionMove pre-scan and in-loop skip (9229-9244, 9270-9276), and the two
  `ss_seed_local_select_node` call sites in addAsFamily/addAsIndividual (1088-1090, 1138-1140). `sendSelect`
  (5551) and all other multicast sends (ObjectPermissions, ObjectOwner, ObjectGroup, ObjectName, ObjectCategory,
  ObjectDetail, ObjectDuplicate*, DeRezObject, ObjectClickAction, ObjectBuy, Undo/Redo) flow through the funnel
  and are thereby gated.
- **lltoolgrab brace structure is correct** — in handleHoverActive the local-content conditional wraps exactly
  the send (732-756); the client-side offset math, edge-of-screen auto-rotate and cursor handling all execute
  for local objects. handleHoverNonPhysical's gate likewise only suppresses the announce (905-938).
- **fscommon::applyDefaultBuildPreferences (214) and fsfloaterimport::setPrimPosition (1156) gates** are at
  function top as intended; fslslbridge.cpp:1197's MultipleObjectUpdate is `setupBridgePrim` on the bridge's own
  attachment prim — unreachable for local scenery.
- **XML/registry consistency**: list columns name/mesh/mode match the code rows (panel_ss_atmo_env_landscape.xml
  34-47 vs ssfloateratmoenv.cpp:1881-1887); button names landscape_select/lock/delete and the landscape_tab panel
  name all match the C++ (`landscape_tab` at floater_ss_atmo_env.xml:735-743, lookup at ssfloateratmoenv.cpp:1840);
  `SSAtmoLandscape` setting entry (settings.xml, after SSAtmo... 25637 block) is well-formed; both new .cpp files
  (CMakeLists.txt:233-234) and headers (1152-1153) are listed in both source and header sections.
- **llviewerdisplay hook placement** — SSAtmoLandscapeWorld::update() (llviewerdisplay.cpp:1005-1007) sits inside
  the standard non-snapshot frame block after the applier and water, matching the documented tick order.
- **Pickability works** (via the bridge, not the new whitelist line): the object is an LLVOVolume with
  `mbCanSelect=true` default (llviewerobject.cpp:278) in the PARTITION_VOLUME bridge, so both cull waves
  (pipeline.cpp:2695-2700) and the pick whitelist (pipeline.cpp:7369) reach it like any prim.