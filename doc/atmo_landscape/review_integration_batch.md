# Review: integration batch (Integrate follow-ups / Fix public-access / Polish)

Commit range: `c6f1a76adf`..`7c5363e6e3` — v1-gap fixes for the Atmo Magic Landscape
feature. REVIEW-ONLY; tree never built. All claims verified by reading the tree.

- llselectmgr.cpp: name/desc write-back (~5291-5390), confirmDelete local branch (~4441-4512)
- lltooldraganddrop.cpp: dad3dApplyToObject full-perm gate (2723-2757)
- ssatmolandscape.{h,cpp}, ssatmolandscapeobject.{h,cpp}: persist, seed, prune, feed, capture, alpha mode
- ssatmoenvasset.h: version bump 1 -> 2

## Fatal

None found. No missing includes, no signature mismatches, no brace/semicolon
breakage. Details under Verified-ok.

## Major

1. **Alpha-only face blocks are silently dropped (feature hole in the new alpha-mode path).**
   `ssatmolandscapeobject.cpp:171-175` — the `empty` test that decides a block is
   "nothing authored" checks texture/material/repeats/rotation/color but NOT
   `mAlphaMode`. A record face authored with only `alpha_mode` set (default/white
   texture, no material — the "make this face blend with no texture" case) hits
   `continue` at :176-179, so `setMaterialParams`/`setTE` never run. Then capture
   (`:272` `te == def`) is material-blind: `LLTextureEntry::operator==`
   (llprimitive/lltextureentry.cpp:190-204) ignores `mMaterial` params, so the face
   reads as default, is skipped, and the next `captureToRecord` writes a smaller
   `mFaces` list — **deleting the authored block from the asset on the first
   reconcile**. Fix: add `&& f.mAlphaMode == 0` to the `empty` test (mirrors the
   TE-default intent), and compare the material params in the `te == def` decision
   instead of relying on `operator==`.

## Minor

1. **"(missing)" marker never appears once the list is drawn** —
   `ssfloateratmoenv.cpp:1852-1894`: `refreshLandscape()` is signature-guarded on
   mesh ids + names + lock states, and `mLandscapeListSignature` is only cleared by
   the delete/lock clicks. Availability flips happen *after* the list is first built
   (header still loading -> 404), but the signature does not include
   `meshKnown()`/`meshAvailable()`, so the suffix stays stale. Fix: append
   `objp->meshKnown() + (meshAvailable()?'1':'0')` for each row to the signature
   (ssfloateratmoenv.cpp:1861-1865).

2. **Availability feed treats "hasHeader=false" as available forever** —
   `ssatmolandscape.cpp:255-260`: `available = !has_header` is an intentional
   "unknown = fine" for a loading mesh, and true 404s DO land a header
   (`mMeshHeader[mesh_id]` with `m404=1`, llmeshrepository.cpp:2389,2396, so
   `hasHeader` true + `getActualMeshLOD` -1 — the detection itself is correct). But a
   mesh whose header fetch is permanently stuck (server blackholing the request,
   no 404, no retry) shows as healthy. Acceptable by design comment; note only.

3. **Write-back is a no-op for records not in the active track** —
   `ssatmolandscape.cpp:48-85`: `ss_landscape_persist_name` only finds the record in
   `primaryTrackIndex()`. Selecting a landscape object after a track switch or from a
   cross-track record leaves the panel's new name/desc on the node while the record
   (and future rehydrations) keep the old value. Fix: fall back to a mesh-id scan
   over all tracks (as `removeByMesh`-style helpers do), or surface a warning.

4. **`applyFaces` re-fires placement on every name edit** —
   `ssatmolandscape.cpp:78` `objp->applyRecord(record)` re-applies position/rotation/
   scale and resets `mAppliedFaces = -1`, so a rename during an in-flight drag on the
   *same* object snaps it to the (<= 0.25 s stale) record transform once. Harmless
   in practice; worth noting the name/desc path only needs the capture baseline
   refresh, not a full re-apply.

## Verified-ok

1. **Write-back branch compiles and type-checks** — `LLObjectSelection::iterator`
   with `begin()/end()` exist: llselectmgr.h:302-304; `(*iter)->getObject()` resolves
   to the non-const overload (llselectmgr.h:205) for non-const iteration. `dynamic_cast`
   uses the complete `SSAtmoLandscapeObject` type: llselectmgr.cpp:56 includes
   ssatmolandscape.h -> ssatmolandscapeobject.h -> llvovolume.h + ssatmoenvasset.h
   (SSAtmoEnvLandscape). `ss_landscape_record_for_mesh`/`ss_landscape_persist_name`
   declarations ssatmolandscape.h:127,134 match definitions ssatmolandscape.cpp:48,93.
   `ssIsLocalContent()` is const (llviewerobject.h:613) so it works through the
   `LLSelectNode::getObject() const` path and the raw `LLViewerObject*` path both.

2. **objectAt() const-status**: non-const (ssatmolandscape.h:65-68). All callers are
   non-const (ssatmolandscape.cpp:75 via `SSAtmoLandscapeWorld*`, ssfloateratmoenv.cpp:1882,
   1943). `ss_landscape_record_for_mesh` (const-correct, ssatmolandscape.cpp:101) does
   NOT call objectAt — no const-caller break. The old private-member `mObjects`
   access from the free function (the 7c5363e fix) is gone; public accessor path is clean.

3. **Availability feed volume safety** — `getVolume()` is `LLPrimitive::getVolume()`
   (llprimitive.h:468) returning the raw `mVolumep`; non-null for every live object:
   the ctor chain runs `applyMesh` -> `setVolume(params,0)` (ssatmolandscapeobject.cpp:106-115)
   before `adoptViewerObject`, so the volume exists before `update()` can ever see the
   object. The dead-prune (ssatmolandscape.cpp:236-242) runs BEFORE the feed (:248-263),
   so killed objects never reach `getVolume()`. 404/purged state arrives via the header
   with `m404` (llmeshrepository.cpp:2389 -> getActualMeshLOD -1, :3614-3617), so the
   feed reports missing/known correctly.

4. **Alpha mode survives setTE into rendering and capture** — `LLMaterialPtr` default
   ctor is null; `new LLMaterial()` is valid (llmaterial.h:36,59). `setTE` ->
   `LLPrimitive::setTE` -> `copyTexture` -> `newCopy()` -> full `LLTextureEntry` copy
   ctor that copies `mMaterial` (lltextureentry.cpp:78-101, llprimtexturelist.cpp:128-143).
   The render path reads it directly: `LLFace::isAlphaBlended` consults
   `te->getMaterialParams()->getDiffuseAlphaMode()` (llface.cpp:1235-1239). Capture's
   `te.getMaterialParams().notNull() ? getDiffuseAlphaMode() : 0` (ssatmolandscapeobject.cpp:282-283)
   round-trips the mode for any face where applyFaces stamped it. `setRenderMaterialID(i, id, false, true)`
   matches llviewerobject.h:212. Only the alpha-*only* block case (Major 1) breaks.

5. **Face perms gate** — placement is after `locateInventory` + finished check
   (lltooldraganddrop.cpp:2737-2738), before `willObjectAcceptInventory` (:2759) and
   before every stock DAD branch (:2760+, :2803+). DAD_MATERIAL funnels through
   dad3dMaterialObject -> dad3dApplyToObject(obj, face, mask, drop, DAD_MATERIAL)
   (:2886-2889), so the gate fires for both texture and material drops before the
   stock material branch. ACCEPT_NO flows untouched for non-local objects
   (`obj->ssIsLocalContent()` gate, :2746). `checkPermissionsSet(PermissionMask)`
   exists (llviewerinventory.h:157); `PERM_ITEM_UNRESTRICTED` defined
   (llinventory/llpermissionsflags.h:75). Hover returns ACCEPT_NO (correct cursor),
   drop shows the alert and rejects.

6. **confirmDelete record removal is safe** — the loop (llselectmgr.cpp:4461-4473)
   is brace/semicolon-clean. `removeByMesh` -> `removeRecord` -> `clearLandscapeObjects`
   -> `gObjectList.killObject` -> `markDead` (llviewerobjectlist.cpp:1598-1623); markDead
   and cleanupReferences never mutate the selection node list ("Don't clean up mObject
   references…" llviewerobjectlist.cpp:1589), and nodes hold `LLPointer<LLViewerObject>`
   (llselectmgr.h:272) so the killed object survives until deselect. Dead nodes are
   pruned by `LLObjectSelection::cleanupNodes()` (llselectmgr.cpp:8362) called from
   `LLSelectMgr::updateEffects()` (:295) — the lingering dead selection is transient.
   The follow-up DeRezObject send is inert for locals: `sendListToRegions`' push_
   functors gate every send type on `!ssIsLocalContent()` (:6085-6114), and select/
   deselect announces are gated too (:521, :992). Iteration itself is safe: the
   boost filter_iterator walks `mList`, which killObject never touches.

7. **update() ordering keeps capture aligned** — order per frame: signature check/
   reconcile (:226-230), applyFacesToAll (:232), dead-prune (:236-242), availability
   feed (:248-263), capture (:269-273). Prune removes corpses BEFORE capture, so after
   a region teardown the sizes can mismatch only transiently and `captureAll`'s
   mesh-match fallback (:370-384) covers it; the size-guarded index pairing (:358-368)
   is exact whenever reconcile and records agree (reconcile builds mObjects strictly
   in record order, :288-312). `removeByMesh`/`removeRecord` (clear + signature-clear,
   :599-601) re-hydrate the next frame through the normal signature path; the
   `editing` deferral (:187-195) prevents mid-drag reshaping; after confirmDelete the
   world's mObjects are empty so the editing scan is false and reconcile runs.

8. **Schema v2 is consistent** — `SS_ATMOENV_VERSION = 2` is the only definition
   (ssatmoenvasset.h:50) and the only version reference: `asLLSD` writes
   `sd["version"] = SS_ATMOENV_VERSION` (ssatmoenvasset.cpp:1848); `fromLLSD` accepts
   version <= 2 and rejects > 2 with "newer than this viewer" (ssatmoenvasset.cpp:1881-1894).
   Nothing hard-codes "1" (the "1" in ssatmostore.cpp is the pre-existing compress
   format marker, unrelated). v1 documents parse as empty-scenery tracks (:1677-1690).
   Face `mIndex` roundtrip: asLLSD writes it only when >= 0 (:1451-1454), fromLLSD
   defaults -1 (:1488), applyFaces positional fallback handles -1 (object.cpp:151), and
   capture always writes explicit indices (:277) — stable either way.

9. **mMeshAvailable/mMeshKnown consistency** — declared/initialised
   (ssatmolandscapeobject.h:97-98), written only through the inline `setMeshAvailable`
   (:69), read via the const getters (:71-72) in ssatmolandscape.cpp:262 and
   ssfloateratmoenv.cpp:1883. No mismatch, no missed initialisation.

10. **Seed/naming plumbing** — `nodep->mPermissions->init(4-arg)` and
    `initMasks(5-arg)` match llpermissions.h:123-127; `mValid`, `mName`, `mDescription`,
    `mCreationDate` are public LLSelectNode fields (llselectmgr.h:229-255); seeding is
    wired into both addAsFamily and addAsIndividual (llselectmgr.cpp:1090, 1140) before
    the node joins the selection. `ss_landscape_item_fullperm` (ssatmolandscape.cpp:87-91)
    and `getIsFullPerm()` are per the panel contract (ssfloateratmoenv.cpp:648-661).

11. **Other API surface** — LLVOVolume 3-arg ctor (llvovolume.h:127), `setVolume`
    (llvovolume.h:236), `getNumFaces`/`getNumTEs`/`getTE`/`getRenderMaterialID(U8)`
    all present; `hasHeader`/`getActualMeshLOD` (llmeshrepository.h:602,605,912);
    `isDead()` (llviewerobject.h:156); `applyFaces`'s index-mapping vs. TE-count guards
    are consistent (object.cpp:147-156, 182-185, 210); capture threshold
    (`num > 0 && mAppliedFaces >= 0`, :264) prevents replacing authored faces with an
    un-applied empty set. Settings.xml declares SSAtmoLandscape (settings.xml:25640-25650),
    the tab is wired (floater_ss_atmo_env.xml:740-742), panel buttons connect
    (ssfloateratmoenv.cpp:132-136). persist_name re-apply refreshes the capture baseline
    (mAuthored via applyRecord) so rename does not fight capture, and the asset save
    path picks the change up through `isModified()` -> full `asLLSD` comparison
    (ssatmoenvmanager.cpp:75-79).