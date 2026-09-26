# Atmo Magic Landscape — Fresh-Eyes Adversarial Review, Loop 2

Scope: commits `47a929a216` + `6b5c119e99`, read from `design/atmo-landscape`.
Prior-round findings (gates, partition revert, node seeding, capture/reconcile, LOD stretch, compile
hazards) are NOT re-litigated. Worktree only, no build, no edits.

---

## Fatal

None found. The closest-to-fatal candidates (mid-frame kill, mesh-repo re-entrancy, region pointer
stability) all resolve to safe due to stock deferral semantics — see Verified-ok.

---

## Major

### M1. `captureAll`/`toggleRecordLock` re-match records by mesh-id instead of by the reconcile pairing — duplicate-mesh replicas lose the second instance's edits and resurrect stale transforms
`ssatmolandscape.cpp:277-296`: `captureAll` loops objects and grabs the **first** record whose
`mMeshId` matches, `break`s, and never pairs like `reconcile` does (`taken[]` consumption,
`ssatmolandscape.cpp:211-226`). Two records sharing one mesh id (an intentional, R1-supported case
per the reconcile comment) therefore capture **both** objects into record[0]: record[1] never
receives a write, and record[0] ends each capture cycle with whichever object wrote last. On the
next reshape (add/delete/track change), `applyRecord` slams **both** replicas back to record[0]'s
transform — the second replica's entire edit history is unrecoverable, and two objects fight over
the same record every 0.25 s. Same first-match bug in the lock toggle re-apply loop,
`ssatmolandscape.cpp:373-381`.
Fix: capture and lock-toggle by **index pairing** (object k ↔ record k, exactly like reconcile
builds `next`), not by mesh-id scan.

### M2. Take / Take Copy / Return / Save-Into-Task funnels are ungated — local objects get DeRezObject'd as if they existed on the sim
`llviewermenu.cpp:6219-6318` `get_derezzable_objects()` has **no** `ssIsLocalContent()` filter; the
seeded full-perm node (`ssatmolandscape.cpp:94-103`) makes every perm check pass, so local scenery
lands in the derez lists. `derez_objects` (`llviewermenu.cpp:6380-6414`) then ships their local ids,
and for `dest != DRD_RETURN_TO_OWNER` increments the window busy count (`6410-6412`) waiting for an
inventory update that can never arrive — a Take on a pure-landscape selection leaves the client
busy-spinning. In a **mixed selection** the packet mixes unknown local ids with real ones; the sim
tears down the whole derez (cross-region style abort), silently failing the Take of the real
objects too. Callers: `6462` (Take), `6750` (Take Copy), `6517`/`7643` (Return×2), `7049`
(SaveIntoTask). This is exactly the "pie-menu Take/Touch/Open items that run regardless of gates"
hole the round-1 gate set did not cover.
Fix: skip `object->ssIsLocalContent()` inside `get_derezzable_objects` (one filter, all five verbs).

### M3. `addFromItem` bypasses the documented editing-deferral contract — a floater add mid-drag snaps the object under the pointer
`update()` defers every reshape while a scenery object is selected so a reconcile cannot snap a
mid-drag object back to the stale record (`ssatmolandscape.cpp:143-155`), and `captureAll` keeps the
drag preserved. But `addFromItem` calls `reconcile()` **directly** (`ssatmolandscape.cpp:454`) with
no editing check; the dragged object is adopted by `applyRecord` (`ssatmolandscape.cpp:228-231`) and
reset to the record's last-captured placement while the mouse is still down. The same direct-call
bypass exists in `removeRecord` (`479`) and, deliberately, `toggleRecordLock` — but add is the one a
dragging author actually hits (drop a second mesh while positioning the first).
Fix: in `addFromItem`, invalidate the signature instead of reconciling inline, and let the
deferral-aware `update()` reshape; or extend the editing check to the add path and queue.

---

## Minor

### m1. Duplicate/repeat-duplicate deselects the selection and produces nothing
`selectDuplicate` (`llselectmgr.cpp:4750-4756`) runs through the gated `sendListToRegions` funnel
(`6024/6041`) — the packet is filtered, **and** the `select_copy` branch unconditionally runs
`deselectAll()`. Ctrl-D on a landscape selection: menu is enabled (`canDuplicate` passes on seeded
full-perm, `8121-8129`), the selection vanishes, no copy exists anywhere. Fix: make `canDuplicate`
(and repeat/on-ray) return false when every root is local, or implement a client-side `createObject`
copy for local roots.

### m2. Edit→Delete on a landscape object is a silent no-op — it can never be deleted with the build tools
`selectDelete`→`confirmDelete`→`sendListToRegions("ObjectDelete", ...)` (gated, `6024`), and
nothing client-side marks the object dead. The only working delete is the floater's
`onClickLandscapeDelete`→`removeRecord` (`ssfloateratmoenv.cpp:1891-1905`). A user who drags scenery
into a wall and hits Delete gets an OK dialog and nothing happens. Fix: in `confirmDelete`, call
`removeRecord`/`killObject` client-side for local roots (and gate the node name with a "removed from
scenery" notice).

### m3. Locked-mode drag across a region border captures a stale-frame offset, and the lock toggle converts it with the wrong region
While editing, the region rebuild is deferred (`ssatmolandscape.cpp:161`) but capture keeps writing
`getPositionRegion()` (`ssatmolandscapeobject.cpp:214-218`) against the **old** region's origin; a
free-fashioned drag across the border (there is no client-side boundary) then persists a locked
offset that re-anchors 256 m off in the new region. `toggleRecordLock`'s locked→free conversion
uses `gAgent.getRegion()` (`ssatmolandscape.cpp:358-360`), amplifying the same skew. Fix: convert
with the **object's** `regionp` and gate the deferral on the object's own region handle.

### m4. Mesh-asset-missing scenery renders as a default **box proxy** with no UI signal
`setVolume` 404 path (`llvovolume.cpp:1283-1289`) turns the sculpt id null and shows the stock
`Inv_Mesh` icon; the floater list keeps showing a normal-looking row, and `applyFaces` transiently
paints authored faces <6 onto the 6-face proxy box (`ssatmolandscapeobject.cpp:130-206`). A purged
or never-uploaded asset therefore degrades to "weird white box in the scenery" with zero diagnostics.
Fix: surface the mesh-header status (`gMeshRepo.hasHeader`) as a list column/warning, and skip
face application while the proxy is up.

### m5. Texture LOD is not stretched — sharp geometry, blurry textures at 1-2 km
The stretch multiplies only calcLOD's geometry distance term (`llvovolume.cpp:1757`); texture
discard selection still derives from the raw drawable distance/texel area, so a 2048 m mountain gets
full-resolution geometry and potato textures simultaneously. Either fold the scale into the face
decode priority (or a per-object minimum discard), or document the asymmetry as intended.

### m6. Landscape panel authored 1180 tall inside a ~725 px tab area — survives only by accident
Sibling panels are 580 tall (`panel_ss_atmo_env_weather.xml:11`); the tab content rect is ~725
(`floater_ss_atmo_env.xml:660-671`). The landscape panel's list (1020), buttons (1040) and hint
(1075) only land inside the visible area because `LLTabContainer::addTabPanel` re-reshapes each
panel to the content rect (`lltabcontainer.cpp:1164-1165`) and the buttons are bottom-anchored.
Works, but the authored geometry no longer means anything; re-author at the container height, or
wrap the list in a scroll container to justify the depth.

### m7. Dead objects linger in `mObjects` and keep being capture/face-processed
Any stock kill of a live object (e.g. a future gate fix, or `clearLandscapeObjects` while selected)
leaves the entry in `mObjects` until the next signature change; `update()`'s editing probe
(`ssatmolandscape.cpp:148-155`), `applyFacesToAll` and `captureAll` run on the dead object with no
`isDead()` check. Harmless today (markDead defers destruction, `llviewerobjectlist.cpp:1609-1618`)
but brittle. Fix: purge `isDead()` entries each update.

### m8. `addFromItem` drops the mesh at scale {1,1,1} — authored size ignored
`ssatmolandscape.cpp:430-448`: no record of the mesh's native size; an uploaded 256 m mountain
appears 1 m tall after its first drop, with only the scale manipulator as guidance. Fix: prescale
from the mesh header's bounds (`gMeshRepo` header/extents) at add time.

### m9. Stale doc: "used by seating"
`ssatmolandscape.h:122-123` claims `ss_landscape_record_for_mesh` serves seating; its only consumer
is the select-node seeder (`ssatmolandscape.cpp:86-88`). Seating was never wired. Fix: update the
comment or wire the intended use.

---

## Verified-ok

- **Send-funnel gates**: select/deselect announcements (`llselectmgr.cpp:521, 992, 1045`), the
  universal `sendListToRegions` collectors (`6024, 6041` — every message name, including
  ObjectDuplicate/ObjectLink/ObjectDelete, flows through them), both property-request windows
  (`6189, 6217`), selection-move packet (`9238, 9275`). Grab/spin/degrab pulses all gated:
  `lltoolgrab.cpp:604, 740, 913, 1208, 1257`. Build prefs (`fscommon.cpp:214`) and importer
  (`fsfloaterimport.cpp:1156`) covered.
- **DAD_MESH cargo**: only `AT_MESH` inventory items map to `DAD_MESH` (`llviewerassettype.cpp:81`);
  links map to `DAD_LINK` (`:78`) and are rejected by the floater. Full-perm gate is the only check
  (`ssatmolandscape.cpp:47-51`) — correct for the threat model.
- **LOD request follows the stretched value** — no high-geometry/low-expectation mismatch: the mesh
  fetch LOD is `mLOD` (`llvovolume.cpp:1235`, `loadMesh` at `1320`), and `mLOD` is assigned from
  the **stretched** distance inside `calcLOD` (`1757 → 1818-1821`). 404 paths render the box proxy
  (never invisible-forever) and `notifyMeshLoaded` (`1453-1456`) rebuilds geometry when the header
  eventually lands. m5's texture asymmetry is the only real LOD gap.
- **Threading/re-entrancy**: `apply()`, `update()` and mesh-repo callbacks all run on the main
  thread; the applier's track reference (`ssatmoenvapplier.cpp:199`) cannot be invalidated mid-frame
  because `editable()` returns the same `mWorking` (`ssatmoenvmanager.h:58-59`) and the frame order
  is fixed (`llviewerdisplay.cpp:998-1007`).
- **Free-mode recreation math**: `createObject` binds the **new** region (`ssatmolandscape.cpp:256`)
  and `setPositionGlobal` (`ssatmolandscapeobject.cpp:124`) re-derives the offset from the stored
  double global — the octree lands at the stored global in the agent's current region.
- **Memory**: `killObject` only marks dead; destruction is refcount-deferred
  (`llviewerobjectlist.cpp:1609-1618`); `mObjects`' `LLPointer`s are the sole ownership, and the
  `adoptViewerObject` failure path deletes the fresh object (`ssatmolandscape.cpp:256-263`).
- **AGENTS.md**: every stock injection carries `<SS:Nexii>`…`</SS:Nexii>` tags (`llviewerobject.h:
  611-615, 830-832`, `llvovolume.h:433-440`, `llvovolume.cpp:1755-1757`, `llselectmgr.cpp`,
  `lltoolgrab.cpp`, `fscommon.cpp`, `fsfloaterimport.cpp`); only SS-prefixed identifiers introduced;
  `SSAtmoLandscape` opt-in defaults off (`settings.xml:25640`); no `rand()`/`ll_rand` in the
  landscape sources.
- **XUI mechanics**: bottom-anchored buttons and hint land inside the re-reshaped tab area
  (`panel_ss_atmo_env_landscape.xml:50-94`); all three buttons wired (`ssfloateratmoenv.cpp:132-137`)
  with sane guards.