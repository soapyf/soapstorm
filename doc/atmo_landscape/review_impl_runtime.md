# Review — Atmo Magic Landscape implementation, runtime core

Commit reviewed: `47a929a216` ("Implement Atmo Magic landscape scenery: schema, local objects, gates, floater").
Files: `ssatmolandscape.{h,cpp}`, `ssatmolandscapeobject.{h,cpp}`, `ssatmoenvasset.{h,cpp}` (landscape schema),
`ssfloateratmoenv.cpp` (landscape tab), stock injections (llvovolume, llselectmgr, lltoolgrab, pipeline,
llviewerregion, llspatialpartition, llviewerdisplay, fscommon).
Method: read every referenced path in the worktree; no build (per project rules).

---

## Fatal

### F1 — The capture funnel silently erases every record's authored faces on any hydration (num-faces-is-0 wipe)

`SSAtmoLandscapeObject::captureToRecord` has no guard for a volume whose geometry has not materialised:
`ssatmolandscapeobject.cpp:222-260`. `num = getNumFaces()` is 0 for a freshly created object until the mesh
loads and the drawable geometry is built (`LLVOVolume::createDrawable` sets `mNumFaces = getNumTEs()` = 0 at
`llvovolume.cpp:1177-1182`; faces appear only after `notifyMeshLoaded` → REBUILD_GEOMETRY,
`llvovolume.cpp:1453-1456`). With `num == 0`:

- the per-face loop writes nothing → `faces.size() == 0`;
- `if (faces.size() != mAuthored.mFaces.size()) changed = true` fires for any record that HAS faces;
- `record.mFaces = std::move(faces)` **replaces the record's authored face list with an empty one**;
- `mAuthored = record` (`:264`) then confirms the empty list, so the wipe is permanent: `applyFaces`
  (`:129-180`) subsequently applies nothing (`max_face == 0`) and capture diffs empty-vs-empty → no rewrite,
  ever.

Timing makes this near-certain: the world's capture timer is a single global tick every 0.25 s
(`ssatmolandscape.cpp:176-180`) that keeps firing regardless of object creation, so every object born after
the first tick (environment load, region change rebuild `ssatmolandscape.cpp:143-149`, track crossing to new
meshes, floater add in `addFromItem` → `reconcile` `ssatmolandscape.cpp:439`) gets a capture inside 250 ms —
well inside the mesh-fetch window even for cached meshes. The 404 path (`llvovolume.cpp:1283-1289`) turns the
volume into a default box; if its faces are default-TE the same wipe fires.

Net effect: per-face texture/PBR-material/tint edits survive only until the next rebuild, and because the
wiped state is persisted to the notecard by the normal save path, the authored look is lost from the asset
itself. This breaks the feature's core persistence promise (`design_synthesis.md` §2, §6, §8) and the
"reconcile funnel" entirely for faces.

Fix: in `captureToRecord`, when `num <= 0` (or while `mAppliedFaces < 0`), skip the faces compare/write
entirely (do not report changed, do not overwrite `record.mFaces`), and keep `mAuthored.mFaces` untouched.

### F2 — Duplicate mesh-id records collapse to one object; pairing is not one-to-one and corrupts the first record's placement

`reconcile`'s UUID adoption (`ssatmolandscape.cpp:190-212`) searches `mObjects` for the **first** object with
a matching mesh id **without consuming the match**. For records R1(m), R2(m) the second iteration finds the
*same* object again, so `next = [o1, o1]`; the kill-surplus pass (`:215-231`) then verifies o1 is kept and
kills the genuinely distinct second object o2. Result: two live records, one object, and o2's placement is
destroyed — the second `applyRecord` also **moves o1 onto R2's placement** (`:206`), and `captureAll`
(`:262-281`) then writes that single object's state into **both** records, permanently merging R1's authored
position into R2's. Placing two copies of the same mesh in different spots (an explicitly acknowledged design
case — `design_synthesis.md` §5: "Duplicates of one UUID within a track pair greedily nearest-first") is
impossible, and the design's promised recovery does not happen because the pairing state is overwritten, not
transient. No crash (killObject → markDead is idempotent, `llviewerobjectlist.cpp:1618`), but silent record
corruption.

Fix: consume the matched object (erase from a working copy of `mObjects` after pairing, gather leftovers to
kill at the end), or pair by index: record i ↔ the i-th unmatched object with that mesh id.

## Major

### M1 — Track crossing is invisible when two tracks share an identical mesh-id run

The reshape signature is a mesh-id run of the active track's records only (`ssatmolandscape.cpp:162-168`).
The applier publishes the track cut via `primaryTrackIndex()` (`ssatmoenvapplier.cpp:193-197`), but the
signature ignores it. Crossing T1→T2 when both tracks list the same meshes in the same order (common: same
scenery, different placements per preset) leaves `sig == mLastSignature` → **no reconcile** → the live
objects keep T1's placement, and `captureAll` (`:262-281`) targets T2's records by mesh id and overwrites
them with T1's coordinates and face state. Crossing back does the same to T1. Each track's authored layout
for that mesh is silently destroyed; the "instant cut" (`design_synthesis.md` §2 §5) never happens.

Fix: include the track index (and/or region handle) in `mLastSignature`/the computed signature.

### M2 — Name/description editing is dead in both directions

- Object-side: the record name/desc are set only at drop (`ssatmolandscape.cpp:415-421`). `applyRecord`
  never writes them onto anything, and `captureToRecord` (`ssatmolandscapeobject.cpp:186-266`) does not
  capture them, so the record never changes after drop.
- Editor-side: the stock General tab routes name/desc through `LLSelectMgr::sendListToRegions`
  (`llselectmgr.cpp:5272-5301`), whose `push_all` functor **drops local-content nodes before any pack
  functor runs** (`llselectmgr.cpp:6019-6034`) — so the change is neither sent nor even applied to the
  object (LLViewerObject has no name store; naming lives in the simulator and the select node). Since the
  node is re-seeded from the record at every selection (`ss_seed_local_select_node`,
  `ssatmolandscape.cpp:78-100`), the General tab displays the stale drop name forever and rejects edits
  silently. The design claimed "General tab reads/writes name/desc … reconcile funnel syncs
  editor↔record both directions" (`design_synthesis.md` §1.3, §6) — neither direction works.
- Floater list impact (attack 7): `refreshLandscape`'s signature includes `mName` (`ssfloateratmoenv.cpp`,
  `refreshLandscape`), which is fine, but since nothing can change it the list name column is immutable;
  the in-world edit that would retrigger it cannot happen.

Fix: capture node-level name/desc into the record (keyed off the seeded node) or intercept the
`ObjectName`/`ObjectDescription` path for local content and write the record directly.

### M3 — A reshape between throttled captures silently discards completed edits

`captureAll` runs at 0.25 s (`ssatmolandscape.cpp:176-180`). Any reconcile-triggering event — floater add
(`:439`), remove, environment load, track crossing that does change the signature — calls
`applyRecord(record)` (`:206`), which re-applies the **stale** record to the object (`snap-back`). The very
next capture then reads `getPositionRegion() == mAuthored.mLockedOffset` (the snap-back itself), sees no
difference, and — per the baseline advance `mAuthored = record` (`ssatmolandscapeobject.cpp:264`) — never
re-writes. An author's move/rotate/scale finished in the window between the last capture and the reshape is
permanently lost, and the object visibly teleports. Same exposure for a freshly dropped texture (record
re-applies over it before the next capture). The capture-before-reconcile ordering within `update()`
(`ssatmolandscape.cpp:151-180`) is correct only when a drag is *still active* (the grab continuously
re-asserts the position); a released edit in that window is gone.

Fix: on reconcile, adopt-a-match by `applyRecord` only when the object was not modified since the record was
last applied (per-object dirty stamp per `design_synthesis.md` §6), or run capture immediately after a
reshape before applying.

### M4 — Tint/alpha-only faces are captured but never re-applied (capture ↔ apply asymmetry)

`captureToRecord` writes a block whenever `te != def || mat.notNull()` (`ssatmolandscapeobject.cpp:226-232`)
— including a face whose only deviation is a non-white color / alpha (texture null, material null).
`applyFaces` then skips exactly such blocks: `empty = f.mTexture.isNull() && f.mMaterial.isNull()`
(`:148-151`) → `continue`. So an authored face tint (color/alpha from the stock face panel) survives in the
record after capture but is silently dropped on the next hydration, while the fixture's color never comes
back. Also `mAlphaMode` is serialized (`ssatmoenvasset.cpp:1473-1476,1486`) but never read by `applyFaces` —
a dead field whose value, if ever set by hand in the notecard, is also lost.

Fix: apply the color/alpha (and alpha-mode where it matters) for texture-less, material-less blocks —
or, inversely, don't write such blocks in capture.

## Minor

### m1 — "Default" sparse comparison uses the LLTextureEntry default ctor, whose id is `LLUUID::null`, not the standard 89556747 texture

`LLTextureEntry()` inits id to `LLUUID::null` (`lltextureentry.cpp:62-76`), and `operator==` compares all
fields incl. `mID` (`lltextureentry.cpp:190-204`). `captureToRecord`'s `def` (`ssatmolandscapeobject.cpp:226`)
therefore does **not** treat a face with the standard default texture `89556747-24cb-43ed-920b-47caed15465f`
as default: such a face is serialized as a full block (`asLLSD` writes `texture`, `repeats`, `color`,
`ssatmoenvasset.cpp:1448-1478`). Round-trip is visually idempotent, so this only breaks the "sparse
face / plain-textured mesh ≈ 40 B per face" budget claim (`design_synthesis.md` §3) — a 89556747-textured
mesh serializes ~ full-size blocks per face and high-face meshes can exceed the 64 KiB notecard ceiling that
`SS_ATMOENV_MAX_LANDSCAPE_*` was derived from. Note also `FSCommon::applyDefaultBuildPreferences` —
the in-world default-texture stamp — is now correctly gated off for local content (`fscommon.cpp:211-216`),
so for the common case (mesh faces carry null or mesh-authored textures) the comparison is right.

Fix: treat `89556747-...` as part of "default" in the capture emptiness test (compare against that id), or
document that the standard texture is an authored deviation.

### m2 — No phantom flag: scenery collides with the avatar

Design §1.1: "Phantom (no collision) via the stock flag; never physics." The ctor
(`ssatmolandscapeobject.cpp:71-90`) sets ownership/perm flags but never `FLAGS_PHANTOM`
(`llviewerobject.cpp`/`flagPhantom`). A dropped piece near the author is a solid static volume in the
client-side avatar physics; walks are blocked. (No server send is involved — collision is viewer-side.)

Fix: `mFlags |= FLAGS_PHANTOM;` (and honor it in the physics shape build).

### m3 — 404/purge handling: "failed state" surface is absent and the 404 box fights the record

On `notifyMeshUnavailable`/bad data the stock path converts the volume to the box proxy with sculpt NONE
(`llvovolume.cpp:1283-1289`); the landscape world keeps the object (mesh id in `mAuthored`), which is
correct, but there is no floater "failed" indicator (design §8 promised one) and the capture funnel treats
the proxy's faces as live data (see F1/M4 interactions) — a 404 mesh's record gets its faces wiped, and if
the asset later becomes fetchable the cached 404 (`getActualMeshLOD` → `header.m404`, `llmeshrepository.cpp:
3610-3652`) prevents recovery by stock means. Acceptable as an asset gap, but the wipe adds silent record
damage on top.

Fix: mark records whose live object is a 404 proxy (e.g. `is404` state), skip capture for them, and show the
state in the landscape list; un-flag when a fetch succeeds.

### m4 — Dead/vestigial pieces

- `SSAtmoLandscapeWorld::invalidate()` (`ssatmolandscape.h:89`) is never called anywhere (no reorder path
  exists yet) — harmless, but the "floater reorder" feature the signature comment mentions is absent, so the
  API lures future misuse.
- `applyMesh` calls `setVolume(params, 0)`; the `detail` argument is entirely unused inside
  `LLVOVolume::setVolume` (`llvovolume.cpp:1229-1236` — `lod = mLOD` is used instead), so the `0` is a
  no-op for mesh params and the LOD chain (`getActualMeshLOD(volume_params, lod)` at `:1242-1253`) is the
  actual gate. Correct, but fragile if stock ever starts honoring the parameter; the call sites in
  `vjlocalmesh.cpp` pass `3`.
- `ss_seed_local_select_node`'s fallback ordering is fine (`init` then `initMasks(PERM_ALL×5)`,
  `llpermissions.h:123-127` semantics), but it stamps `gAgentID` as owner unconditionally — an environment
  authored elsewhere opens as owned by the local agent for editing purposes; acceptable for local-only
  content, just surprising for Reveal/Inspect.
- The `LLVector3d → LLVector3` fallback in `toggleRecordLock` (`ssatmolandscape.cpp:351-352`) truncates a
  global to a region-sized float when the region is null (teleport frames); a few-hundred-metres error is
  possible in the pathological case.
- `setScale` in `applyPlacement` (`ssatmolandscapeobject.cpp:126`) can push owned, large volumes onto the
  minimap via `LLViewerObject::setScale` → `addToMap` (`llviewerobject.cpp:4197-4224`) — scenery dots appear
  on the map. Cosmetic; verify intent.

## Verified-correct highlights

- **No dangling record references, no double-adoption via pointers.** Objects copy their record
  (`mAuthored = record`, `ssatmolandscapeobject.h:86`, ctor `applyRecord`); `captureAll` re-matches records
  by mesh id inside the tick rather than caching pointers (`ssatmolandscape.cpp:262-281`); `recordAt`/`
  ss_landscape_record_for_mesh` return transient pointers into `mWorking` used only synchronously in
  single-threaded paths. `reconcile` builds `next` strictly in record order, so `objectAt(i)` ↔ `recordAt(i)`
  stays aligned (`ssatmolandscape.cpp:187-233`).
- **Feedback loop is genuinely dead in steady state.** The reshape signature covers only mesh ids
  (`ssatmolandscape.cpp:162-168`), capture writes do not trip it; `applyFaces` runs before `captureAll`
  each frame (`:174-180`); the baseline advance (`mAuthored = record` after capture) plus epsilon
  comparisons (`near_v3/1e-4`, `near_quat`, `face_equiv`) make an untouched object report unchanged forever.
  The failures above (F1, M1, M3, M4) are all *asymmetries around* that loop, not the loop itself.
- **Region-change rebuild ordering is sound.** Handle change → clear → signature clear → reconcile →
  applyFaces → capture all within one `update()` (`ssatmolandscape.cpp:143-180`), so capture can never read a
  position in the old region's frame; locked records re-anchor by construction and capture diffs
  region-local offsets (`ssatmolandscapeobject.cpp:187-193`), which is the correct region-aware behaviour
  the design demanded.
- **Mesh-volume plumbing matches stock.** `setSculptID(id, LL_SCULPT_TYPE_MESH)` + `setVolume(params, …)`
  is exactly the stock mesh path (llmeshrepository.cpp:2648 local physics shapes; vjlocalmesh.cpp:155;
  llpanelobject.cpp:1742), `isSculpted()/isMesh()` check the sculpt params incl. this object's
  (`llvovolume.cpp:3832-3860`), `getActualMeshLOD` handles missing/purged headers gracefully by returning
  the requested LOD until the header parses (`llmeshrepository.cpp:3592-3607`).
- **LOD stretch is correctly folded, stock-safe.** `calcLOD` is virtual with a default 1.0 hook
  (`llvovolume.h:433-441`) and the multiply at the `sDistanceFactor` line is the only change
  (`llvovolume.cpp:1755`); DebugObjectLODs, Mesh-detail preference and the volume face pools are untouched,
  and the `SSLandscapePartition` keeps `RENDER_TYPE_VOLUME` (`llspatialpartition.h:707-732`) so pools/culls
  key correctly.
- **Partition alignment is exact.** `PARTITION_LANDSCAPE` inserted after `PARTITION_VOLUME`
  (`llviewerregion.h:97-110`) and the `initPartitions` push_back in the same order
  (`llviewerregion.cpp:727-733`); `lineSegmentIntersectInWorld` and `rebuildDrawInfo` enumerate it
  (`pipeline.cpp:7367-7380, 12841-12847`).
- **Gate set is coherent for the sends it covers.** The `sendListToRegions` funnel functor
  (`llselectmgr.cpp:6019-6034`), the select/deselect announcements, properties requests, `sendSelectionMove`,
  and grab updates are all gated; `setTE`/`setRenderMaterialID(...,false,false)` are local-only
  (`llviewerobject.cpp:5615-5632, 7957`), and `applyDefaultBuildPreferences` is skipped for local content
  (`fscommon.cpp:211-216`), so the local-content object emits no server traffic through these paths.
- **Caps are enforced at the one add funnel** with per-track 16 and per-asset 32 checks plus user-facing
  reasons (`ssatmolandscape.cpp:396-410`), and no other code path appends records; `fromLLSD` deliberately
  tolerates over-limit notecards without clamping (documented budget decision, `ssatmoenvasset.h:78-83`).
- **Full-perm drop gate is honest.** `ss_landscape_item_fullperm` uses the inventory item's own
  `getIsFullPerm` (`ssatmolandscape.cpp:47-51`) and both the floater drop and the record add share it; the
  floater's failure paths surface the reason string.
- **LLVOVolume ctor prerequisites are met.** `adoptViewerObject` before `gPipeline.createObject`
  (`ssatmolandscape.cpp:241-248`) mirrors the stock tail-minus-local-id registration
  (`llviewerobjectlist.cpp:2224-2261`); the ctor body sets ownership/perm flags before adoption
  (`ssatmolandscapeobject.cpp:81-87`) so `permMove/permModify/permCopy` (`llviewerobject.cpp:7198-7330`)
  pass; mesh requests issued inside the ctor (`applyMesh`) cannot deliver before the drawable exists
  (network async), so `notifyMeshLoaded`'s `gPipeline.markRebuild` always races safe.
- **Per-frame signature cost is negligible** (≤16 × 36-char concatenations per frame,
  `ssatmolandscape.cpp:162-167`); capture is the 0.25 s throttle and the per-face work is small. The
  floater guard (`refreshLandscape` unsigned signature, tab-visibility check) prevents rebuild-during-scroll.