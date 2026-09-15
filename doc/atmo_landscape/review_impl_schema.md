# Adversarial review: "Implement Atmo Magic landscape scenery" (47a929a216)

Scope: schema/serialization, applier publication, landscape world lifecycle, notecard budget, build file placement, partition/pick/shadow integration, compile-risk sweep. Read-only review of commit 47a929a216; worktree `D:\soapstorm\design-worktree`.

---

## Fatal

### F1. `LLVOVolume::calcLOD`/`ssLODDistanceScale` are commented-out virtuals — override is a hard compile error and the hook is inert
`indra/newview/llvovolume.h:438-439` declares
```cpp
/*virtual*/ bool calcLOD();
/*virtual*/ F32 ssLODDistanceScale() const { return 1.f; }
```
The `virtual` keyword is inside a comment, so both functions are NON-virtual. Consequences:
- `indra/newview/ssatmolandscapeobject.h:78` declares `F32 ssLODDistanceScale() const override` against a non-virtual base member → ill-formed on every conforming compiler ("marked 'override' but does not override") → the feature does not build.
- Even with `override` deleted, `indra/newview/llvovolume.cpp:1757` (`distance *= sDistanceFactor * ssLODDistanceScale();`) is a non-virtual call resolved to the base at compile time — the derived 0.125× stretch would silently never apply. The LOD-range feature would be dead code.

Fix: drop the comment markers so `calcLOD` and `ssLODDistanceScale` are actually virtual (keep the derived override), or remove `override` and re-specify the hook as a final/no-virtual extension point: `ssLODDistanceScale` cannot work non-virtual.

### F2. `nodep->getObject().isNull()` on a raw pointer — compile error
`indra/newview/ssatmolandscape.cpp:80`:
```cpp
if (!nodep || nodep->getObject().isNull() || !nodep->getObject()->ssIsLocalContent())
```
`LLSelectNode::getObject()` returns `LLViewerObject*` (raw; `llselectmgr.h:205` non-const, `:278` const) — `.isNull()` on a raw pointer does not compile (the `LLPointer` member waits behind an implicit conversion that never fires here). C2228/C2819-class error on MSVC, same on clang/gcc.

Fix: `if (!nodep || !nodep->getObject() || ...)`.

### F3. Sparse "faces" array is positional but capture compacts it — face assignments scramble to lower indices
The face list has no per-face index (`ssatmoenvasset.cpp:1598-1610` parses it positionally; `ssatmoenvasset.h:133-135` documents "index must be < loaded mesh's face count"). Two writers disagree about sparsity:
- `SSAtmoLandscapeObject::applyFaces` (`ssatmolandscapeobject.cpp:141-177`) treats `mFaces[i]` as the state of TE `i` (positional).
- `captureToRecord` (`ssatmolandscapeobject.cpp:222-240`) builds the list by iterating ALL faces and pushing only the non-default ones — i.e. it COMPACTS. Authoring only face 3 of an 4-face mesh yields `mFaces = [f3]`, which `applyFaces` then applies to TE 0.

Trace: paint only TE3 → capture writes `[f3]` (and the asset/save is contaminated) → next `applyRecord` (reconcile, lock toggle, region change, load/revert — or the object is just re-hydrated from the saved notecard) applies `f3` to TE0. The texture visibly jumps to the wrong face, and the wrong assignment is then persisted. Only contiguous authoring starting at face 0 (or authoring every face) round-trips correctly. This is a data-integrity bug in the feature's core promise ("body captures back what the author edits").

Fix: store an explicit face index in each block (e.g. `sd["face"] = i`), and match by it in both `applyFaces` and the capture/parse paths; or keep dense placeholders. Both writers must agree on the sparsity model.

---

## Major

### M1. Notecard budget claim is false on its own terms; the real headroom comes from a serialization the claim never mentions
- Claim (`ssatmoenvasset.h:78-83`): "a fully loaded record is ~2 KB of pretty XML, so worst-case 16 per track and 32 per asset stay inside the 64 KiB notecard ceiling".
- Reality: the ceiling is `LLNotecard::MAX_SIZE = 65536` (`llnotecard.h:41`); the notecard payload is `toBinary` → zlib deflate (best) → base64+wrap (`ssatmoenvmanager.cpp:368-410`). Pretty XML is only used for the uncapped debug cache dump (`ssatmoenvmanager.cpp:294-343`).
- The pretty-XML arithmetic doesn't hold: an 8-face record serializes to roughly 4-4.5 KB pretty XML (each face ≈ 0.5 KB), so 32 records ≈ 145-160 KB — 2.5× over the ceiling. Under the header's own premise the caps would NOT fit; they only fit because the real path is compressed binary (~1.4 KB/record → ~0.8 KB deflated+b64 ≈ 34-42 KB for 32 records), a fact the comment omits.
- The `64 KiB` figure is exactly hit by the claim's own numbers (32 × 2 KB), with zero headroom for the non-landscape payload, base64 inflation and the notecard export wrapper — the margin that actually exists is accidental.

Fix: rewrite the comment around the real serialization (binary + deflate + base64, save gate at `ssatmoenvmanager.cpp:391`), and state measured per-record bytes; keep the caps as a UX/memory budget, not a "notecard ceiling" justification.

### M2. Oversize saves fail with a log line only; caps are not enforced on the load path
- `updateExistingNotecard` (`ssatmoenvmanager.cpp:1133-1139`) reports serialization failure via `LL_WARNS` and returns — the floater's Save flow gets no feedback, the environment silently stops persisting.
- `SSAtmoEnvTrack::fromLLSD` (`ssatmoenvasset.cpp:1673-1685`) accepts an unbounded `landscape` array — the per-track/total caps (`SS_ATMOENV_MAX_LANDSCAPE_*`) are enforced only in the floater add path (`ssatmolandscape.cpp:396-410`). A notecard carrying 50+ records loads fine and then cannot be saved.

Fix: enforce the caps (or clamp with a warning) in `SSAtmoEnvTrack::fromLLSD`/`SSAtmoEnvAsset::fromLLSD`, and surface save errors through the floater (reuse the status line the save flow already owns).

### M3. Schema version not bumped — an older build silently destroys landscape data
`SS_ATMOENV_VERSION` stays 1 (`ssatmoenvasset.h:48`). Old viewers parse the map and ignore the unknown `landscape` key (tolerant `fromLLSD`), but any later save from an older build rewrites the notecard without the key — scenery is silently dropped, with no version gate to stop it. The in-file versioning exists solely to reject "newer than this viewer" assets (`ssatmoenvasset.cpp:1883-1889`), which is exactly the case this change creates.

Fix: bump `SS_ATMOENV_VERSION` to 2 so pre-landscape builds refuse the asset instead of mutilating it (or treat the version check as a soft downgrade with an explicit warning).

---

## Minor

1. **CMake placement** (`CMakeLists.txt:233-234`): `ssatmolandscape.cpp/.h` were inserted between `sswater` and `sswindflow`, ~40 lines after the `ssatmoenv*` cluster they belong to, and after `sswater` they alphabetically precede. The ss block is not strictly sorted, so harmless, but the pair doesn't sit with its family.
2. **"Develop menu" hint is fiction** (`panel_ss_atmo_env_landscape.xml:88-95`): the hint says the gate is "SSAtmoLandscape in the Develop menu"; the setting exists only as a debug setting (`settings.xml:25640-25651`, no menu XML anywhere). Either add the menu toggle or reword the hint.
3. **Per-track cap is redundant**: with the total cap at 32 (`ssatmoenvasset.h:83`) and 8 tracks, `SS_ATMOENV_MAX_LANDSCAPE_PER_TRACK = 16` can never bind once more than two tracks carry scenery — the total cap is the only real bound. Harmless, but the header presents both as load-bearing.
4. **`mFaces` EOF-doc drift**: header says "dropped on reconcile" (`ssatmoenvasset.h:133-135`) while `captureToRecord` compacts on capture — the doc describes the positional-vs-sparse contradiction (F3) as intended.
5. **`onClickLandscapeSelect` selects into a possibly-stale index**: `objectAt(index)` indexes the live set, which can lag the record list for one frame after an add (reconcile hydrates in `addFromItem`, so mostly fine); the unchecked `(void)handle` plus a deselectAll first makes the click-focus path fragile. Rely on a mesh-id lookup instead of the index.

---

## Verified-correct highlights

- **`primaryTrackIndex()` publication is sound** (`ssatmoenvapplier.cpp:197`): set immediately after the resolver's `blend.mPrimaryTrack` clamp to `[0, size)` in `apply()`, before `applySky`; getter returns -1 whenever `mActive` is false (`ssatmoenvapplier.h:180`), so inactive reads can't leak a stale index; the landscape tick runs after the applier in `display()` (`llviewerdisplay.cpp:1007`), giving it the same frame's cut.
- **`editable()` swap safety holds**: `mWorking` is replaced only in synchronous main-thread handlers — `revertToBaseline` (`ssatmoenvmanager.cpp:82-86`), `adoptCreated` (:1071-1081), `adoptParsedAsset` (:1321-1339), `unload` (:1304-1318), `saveNotecard` (:1097-1099). Landscape `update()` holds `records` only within the frame (`ssatmolandscape.cpp:151-179`) — no reference survives across a swap, and capture writes are value-writes into existing elements. Safe by construction on the single-threaded frame loop.
- **Partition integration is complete and consistent**: enum added after `PARTITION_VOLUME` (`llviewerregion.h:101`) with matching push order (`llviewerregion.cpp:731`); `NUM_PARTITIONS` follows automatically; pick whitelisted at `pipeline.cpp:7371`; highlight-transparent walk at `pipeline.cpp:12845`; rect-select "grow" iterates ALL partitions and only requires `pcode == LL_PCODE_VOLUME` (`llglsandbox.cpp:240-242`) — the new objects pass. Shadow maps are partition-agnostic for the volume family (`mDrawableType` stays `RENDER_TYPE_VOLUME`, `llspatialpartition.h:722`), and the infinite far clip has precedent in water/terrain (`llvowater.cpp:298`, `llvosurfacepatch.cpp:984`).
- **Permissions seeding arg order is correct for this tree**: `LLPermissions::init` is `(creator, owner, last_owner, group)` here (`llpermissions.h:123`) and `ss_seed_local_select_node` passes `(record->mCreator, gAgentID, record->mLastOwner, null)` (`ssatmolandscape.cpp:98`). Correct.
- **Compile-risk sweep** (item 7) — all verified present and correctly used: `LLInventoryItem::getDescription/getPermissions().getLastOwner/getCreationDate` (`llinventoryitem.h`), `gAgent.getPositionRegion`, `LLScrollListCtrl::getFirstSelectedIndex` (`llscrolllistctrl.h:300`), `LLSelectMgr::selectObjectOnly(LLViewerObject*, face, gltf_node, gltf_primitive)` single-arg call (`llselectmgr.h:613`), `LLPanel::getVisible`, `LLNotificationsUtil::add("GenericAlert", …)` (`notifications.xml:166`), `setRenderMaterialID(S32, LLUUID, bool, bool)` / `getRenderMaterialID(U8)` (`llviewerobject.h:206/212`), 3-arg `LLVOVolume` ctor (`llvovolume.h:127`), `LLSelectNode::mPermissions` allocated in ctor (`llselectmgr.cpp:7156`), `LLNotecard::MAX_SIZE = 65536` (`llnotecard.h:41`).
- **Serialization type-correctness** (item 1): `mRotation.mQ[VX..VW]` (LLQuaternion mQ[4]), `mFreeGlobal.mdV[VX..VZ]` (LLVector3d F64), `(LLSD::Real)` casts for F32/F64, `(LLSD::Integer)` casts — all correct; every array read is length-guarded (`ssatmoenvasset.cpp:1490, 1500, 1565, 1573, 1581, 1592`); `LLVector4` repeats round-trip as 4 reals (S,T offsets in mV[VZ], mV[VW], matching TE layout); `fromLLSD` tolerates missing blocks; the `landscape` key is unique in the track map and written only when non-empty (clean sparse default).