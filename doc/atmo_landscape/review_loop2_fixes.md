# Adversarial review — "Fix review-loop-1 findings" (6b5c119e99)

Scope: verify each claimed loop-1 fix in the CURRENT worktree, find new regressions the fixes
introduced, and re-sweep residuals. Read-only review of commit `6b5c119e99` on top of
`47a929a216`; worktree `D:\soapstorm\design-worktree`. No builds performed (project rules).

Method note: `ssatmolandscape.cpp:174` fails to compile as written (see NEW-1) — every fix that
lives in that file is verified at read level only; the file has not built since `47a929a216`.

---

## Fix verified

| # | Loop-1 finding claimed fixed | Current-code verdict |
|---|---|---|
| 1 | Schema F1 — `calcLOD`/`ssLODDistanceScale` were commented-out virtuals; derived `override` was a hard compile error and the hook inert | **VERIFIED.** `llvovolume.h:438-439` are now genuine `virtual` (no comment markers); derived override `ssatmolandscapeobject.h:75` is well-formed. The scale is single-sited: `llvovolume.cpp:1757` `distance *= sDistanceFactor * ssLODDistanceScale();` is the ONLY distance multiply — the near-ramp boost (:1759-1767), `F_PI/3` (:1770), `computeLODDetail` (:1792) and `mLODAdjustedDistance` (:1778) all flow through the scaled value; `mLODDistance` (:1733) and `mAppAngle` (:1820) stay unscaled but are consumed only by debug text and LLVOTree. Virtual dispatch reaches the 0.125× stretch for landscape objects (calcLOD is called on `this`). No ABI impact (class already virtual). |
| 2 | Runtime F1 + Surgery M-2 — capture funnel wiped authored faces pre-mesh (`num==0` → empty list → `record.mFaces = {}` → baseline-advance confirmed the wipe) | **VERIFIED.** Guard at `ssatmolandscapeobject.cpp:251` `if (num > 0 && mAppliedFaces >= 0)` encloses ONLY the faces block (positions/rot/scale capture above it, :212-243). `record.mFaces` is never touched while the mesh is missing. Baseline advance `mAuthored = record` (:296) copies a record whose faces were untouched, so it cannot resurrect a stale/empty set: pre-mesh captures diff empty-vs-empty (no churn) and the next post-mesh capture writes the live TE state only after `applyFaces` has run (`applyRecord` resets `mAppliedFaces=-1` at :102; `update()` runs `applyFacesToAll` before the capture tick, ssatmolandscape.cpp:192-198). No window exists where capture could fire between reconcile/applyRecord and the same-frame `applyFacesToAll`. |
| 3 | Runtime F2 — duplicate mesh-id records collapsed onto one object, corrupting the first record's placement | **VERIFIED in `reconcile`.** Consume-pairing `taken[]` (`ssatmolandscape.cpp:211-226`): each live object matched at most once, in record order; two records with the same mesh → second iteration finds no untaken match → `createObject` (:234) → `next = [o1, o2]`, two live objects. Kill loop (:240-246) kills only `!taken[oi]` objects. `mObjects = std::move(next)` (:248) preserves record order so `objectAt(i) ↔ recordAt(i)` holds (floater's index pairing intact, ssfloateratmoenv.cpp:1937). **BUT capture-side pairing was not fixed — see NEW-2.** |
| 4 | Runtime M1 — track crossing with identical mesh-id runs was invisible (no reconcile, stale capture overwrote the new track) | **PARTIAL / HOLE REMAINS.** Signature now folds the track: `sig += llformat("t%d;", track)` (`ssatmolandscape.cpp:180`) — crossing T1→T2 changes `sig` → reconcile fires. The editing-deferral (`editing` scan :147-155) gates BOTH the region-rebuild (:161) and the reshape (:186) — matching the commit's claim. **The hole:** capture is NOT gated. While `editing` is true and a track crosses, `captureAll(records)` (:197) targets the NEW track's `records` vector (:174) while `mObjects` still mirrors the OLD track's records. Full trace below in NEW-3. |
| 5 | Surgery M-1 + Schema F2 — seed keyed on the object's random UUID (never matched); `getObject().isNull()` on a raw pointer (compile error) | **VERIFIED.** `ssatmolandscape.cpp:80` is now `!nodep || !nodep->getObject() || ...` (raw-pointer correct). Lookup at :86-88 via `dynamic_cast<const SSAtmoLandscapeObject*>(nodep->getObject())` + `ss_landscape_record_for_mesh(landscape->meshId())` — keyed on the mesh asset uuid, the same key records are stored under (`mAuthored.mMeshId`). Include chain compiles conceptually: `ssatmolandscape.cpp → ssatmolandscape.h:34 → ssatmolandscapeobject.h → llvovolume.h/ssatmoenvasset.h` (complete type available for the cast; RTTI is enabled — stock `dynamic_cast<LLVOVolume*>` precedents at llselectmgr.cpp:2740/2766/8610). Call sites still planted: llselectmgr.cpp:1090 (addAsFamily) and :1140 (addAsIndividual). |
| 6 | Surgery minor — `push_some` unfiltered (ObjectBuy, Undo/Redo carried local LocalIDs) | **VERIFIED.** `push_some::apply` now gates `node->getObject() && !node->getObject()->ssIsLocalContent()` (`llselectmgr.cpp:6041-6048`); both the root and child variants are the SAME functor with the `mRoots` flag (:6043-6044), so the single gate covers SEND_ONLY_ROOTS and both SEND_ROOTS_FIRST/SEND_CHILDREN_FIRST halves. Early-out intact: `if (nodes_to_send.empty()) return;` (:6087-6090) fires BEFORE `gMessageSystem->newMessage` (:6100) — an all-local selection sends NO message header and no empty per-region headers. |
| 7 | Surgery minors — ObjectSpinUpdate sent for local objects; ObjectGrab/ObjectDeGrab ungated | **VERIFIED, braces balanced at each site.** Spin: the `if (!(objectp && objectp->ssIsLocalContent()))` at lltoolgrab.cpp:604 opens and closes at :617, wrapping exactly the `ObjectSpinUpdate` send (:607-615) — offset math and edge-of-screen rotate stay client-side. Grab: top-of-function gate `if (!object || object->ssIsLocalContent()) return;` at :1208-1211. DeGrab: gate at :1257-1260 placed after the FS null/region guard — balanced. Pre-existing update gates still present (:740, :913). |
| 8 | Surgery F-1 — the SSLandscapePartition gambit was inert (bridge always wins) | **VERIFIED (option (a) adopted).** `SSLandscapePartition`, `PARTITION_LANDSCAPE` and the two pipeline whitelist lines are gone (`llspatialpartition.h`, `llviewerregion.h/.cpp`, `pipeline.cpp`); `getPartitionType()` override removed (`ssatmolandscapeobject.h`). Grep of all of indra finds zero dangling references. Enum/vector/pipeline order back to stock. Net effect and tradeoff (scenery culls at DrawDistance like any prim; the LOD stretch only smooths pop within draw distance) is the accepted outcome the review offered. |
| 9 | Schema M1 — caps 16/32 claimed safe under a false budget | **VERIFIED with caveats.** Constants now `SS_ATMOENV_MAX_LANDSCAPE_PER_TRACK = 8`, `SS_ATMOENV_MAX_LANDSCAPE_TOTAL = 12` (`ssatmoenvasset.h:83-84`) and `addFromItem` enforces both via the new constants (`ssatmolandscape.cpp:411,421`). Arithmetic: 12 × 4.5 KB = 54 KB < 65 536 ✓ — under the stated "40-face fully authored" premise. The real serialization is binary+deflate+base64 (`ssatmoenvmanager.cpp:373-391`) which is smaller, so the margin is genuine. Caveats: (a) the 40-face figure is not a hard bound — `mFaces` is unbounded (`SSAtmoEnvTrack::fromLLSD` still tolerates any count, ssatmoenvasset.cpp:1679-1690) and `getNumFaces()` can exceed 40 on dense meshes; (b) the new comment still justifies via pretty XML though the notecard path never uses it — the exact lapsus schema-M1 flagged; (c) oversize saves still fail log-only (`updateExistingNotecard` → LL_WARNS + return, ssatmoenvmanager.cpp:1133-1139), the floater gets no feedback. |
| 10 | Schema F3 + runtime M4 — sparse faces were positional while capture compacted; tint/alpha-only blocks captured but never applied | **VERIFIED.** `mIndex` field (`ssatmoenvasset.h:98`, default -1); capture stamps `f.mIndex = i` (`ssatmolandscapeobject.cpp:264`); `applyFaces` builds an index map (`by_face`, :146-155) — out-of-range indexes are bounds-guarded (`face_index < num`), −1 (hand-edited positional) falls back to array position, duplicate indexes last-wins. Diff converges: capture writes indexes in TE order, so the first capture normalizes any hand-authored ordering and steady-state `face_equiv` (which now compares `mIndex` first, :60) is stable — no per-tick churn. M4: the `empty` test now checks repeats/rotation/color too (:170-174), so tint-only blocks are applied (color) and captured — fixed for color/alpha; `mAlphaMode` remains a dead serialized field (`applyFaces` never reads it) — residual. |

---

## NEW bugs introduced

### NEW-1 (CRITICAL) — `ssatmolandscape.cpp:174` cannot compile: non-const `records` bound through a const `asset`
`update()` declares `const SSAtmoEnvAsset& asset = mgr->editable();` (`ssatmolandscape.cpp:168`,
`editable()` returns `SSAtmoEnvAsset&`, ssatmoenvmanager.h:59) and then binds
`std::vector<SSAtmoEnvLandscape>& records = asset.mTracks[static_cast<size_t>(track)].mLandscapes;`
(`ssatmolandscape.cpp:174`). Binding a non-const lvalue reference to the const-qualified member of a
const object is ill-formed on every conforming compiler (MSVC C2440 "cannot convert from 'const
std::vector<...>' to 'std::vector<...>&'"; GCC/Clang "discards qualifiers"). Every other function in
the file correctly uses `SSAtmoEnvAsset& asset` first (toggleRecordLock :342, addFromItem :402,
removeRecord :466) — only `update()` has the `const`. This predates the loop-1 commit (identical
lines in `47a929a216`), but the loop-1 diff EDITED this exact function (the editing-deferral block
sits six lines above the bad binding) without noticing — so the headline "fix" commit cannot be
built or run. All fixes in this file are therefore unverifiable by execution until this compiles.
(Trivial fix: drop the `const` on line 168, matching the rest of the file.)

### NEW-2 (MAJOR) — `captureAll` pairing is still greedy-first-match: with duplicate meshes the second record is frozen and the first receives both objects' state
The F2 fix made duplicate-mesh records first-class in `reconcile`, but the capture side was not
consumed to match. `captureAll` (`ssatmolandscape.cpp:277-296`) loops objects and takes the FIRST
record whose `mMeshId` matches, `break`ing out:

- object[0] (mesh m) → record[0]; object[1] (mesh m) → **record[0] again**. record[1] never
  receives its own object's captures — the second record's authored placement/faces are frozen at
  their drop values while the live object is edited, and the stale content is persisted on save
  (matching the M1-corruption class the F2 fix was written to remove — just moved from record[0]
  to record[1]).
- record[0] receives state from BOTH objects: after object[1]'s capture writes object[1]'s position
  into record[0], object[0]'s next capture diffs against its own `mAuthored` (now carrying
  object[1]'s state) and rewrites — record[0] alternates between the two objects' state each
  0.25 s tick whenever they differ; neither ever converges. Save timing decides which one sticks.

Fix direction: pair capture by index (object i ↔ record i, as the reconcile already does) or
consume matches with a per-record `taken[]` mirroring the reconcile.

### NEW-3 (MAJOR) — the editing-deferral reopens Runtime-M1's corruption for the whole selection duration
`update()` gates reconcile and the region rebuild on `editing` (`ssatmolandscape.cpp:161,186`) but
NOT `captureAll` (:194-198). Trace — track crossing while an object is selected/dragged:

1. Object O (mesh m) mirrors OLD track record R1 (placement P1); author starts dragging O,
   `editing = true` (:147-155).
2. Applier cuts to track 2 → `records` (:174) is now the NEW track's vector (R2 for mesh m, P2).
   `sig` ("t2;m;…") != `mLastSignature` ("t1;m;…") → reconcile would fire, but `editing` →
   deferred; `mLastSignature` stays stale (correct for the release).
3. `captureAll(records)` (:197) matches O to R2 by mesh id and, because the drag moved O off P1,
   **overwrites R2's authored placement/faces with O's live T1 state** — exactly M1's "the new
   track's authored layout is silently destroyed", now gated behind a selection.
4. Author releases: reconcile runs; O adopts R2 — no visible snap, because R2 now holds O's own
   captured position. R1 keeps its last pre-drag capture, so the drag is lost from the OLD track
   too: the edit landed in neither track the way the author intended, and the new track's layout
   is corrupted. Face edits (texture panel re-tint while selected) propagate the same corruption.
5. If the NEW track does not contain mesh m at all, `captureAll` finds no record; the OLD object's
   in-window delta targets nothing and is lost on release (reconcile kills it, un-captured).
6. Secondary: while deferred, the floater's index pairing lies — the list shows the NEW records
   (`recordAt(i)`) while `objectAt(i)` returns OLD-track objects (`ssatmolandscape.h:65-68`),
   so Select/Delete/Lock act on the wrong object for the deferred window (out-of-bounds → `nullptr`
   → no-op when the record counts differ).

The deferral also does not cover the direct reshape paths that bypass `update()`: `addFromItem`
calls `reconcile` inline (`ssatmolandscape.cpp:454`) and `toggleRecordLock` calls `applyRecord`
inline (:377) — both snap a mid-drag object back to the stale record before the next capture tick
(the original M3 exposure, still live on these paths).

### NEW-4 (MINOR-LATENT) — `captureAll` const signature mismatch between header and TU
`ssatmolandscape.h:95` declares `void captureAll(const std::vector<SSAtmoEnvLandscape>&)`;
`ssatmolandscape.cpp:277` DEFINES `void SSAtmoLandscapeWorld::captureAll(std::vector<SSAtmoEnvLandscape>&)`
— a different overload, never declared in the header. It links today only because the sole caller
(`update()`) resolves the non-const overload by argument type within the same TU. The header's
const version is declared but never defined: the moment records at :174 becomes const (the evident
intent behind both the `const` on :168 and this signature), the call switches to the undefined
overload → link error. One or the other is lying; they must agree.

### NEW-5 (MINOR) — `toggleRecordLock` still matches the first mesh-id object
`ssatmolandscape.cpp:373-380` re-applies the toggled record to the FIRST object whose mesh id
matches. With F2 now supporting two records of the same mesh, toggling record[1]'s lock re-applies
record[1] onto object[0] (the wrong instance), leaves object[1] in the old mode, and the next
capture writes the stale-mode position back into the toggled record (via the NEW-2 pairing defect,
into record[0]'s row instead). The index-based pairing is right in reconcile; the two direct
manipulators (lock, capture) must use it too.

---

## Residual issues (loop-1 findings NOT addressed by 6b5c119e99)

- **Runtime M2 (major): name/description editing dead in both directions.** Object-side capture
  never writes `mName`/`mDesc`; editor-side the General tab still routes through the now-fully-gated
  funnel (`push_all`/`push_some` both drop local nodes, llselectmgr.cpp:6024/6041), so name edits
  are silently discarded and the seeded node shows the drop-time name forever.
- **Surgery M-3 (major): derez/Take/Return ungated; pie-menu Delete is inert.** `llviewermenu.cpp`
  has zero `ssIsLocalContent` checks (`get_derezzable_objects` :6217) — Take/Return-to-owner composes
  DeRezObject with the fake LocalID; Delete/Trash sends nothing (funelled), object stays in place,
  no client-side kill, no floater-side removal. Neither the gate nor the "Delete ⇒ removeRecord"
  escape hatch the review proposed exists.
- **Runtime M3 remainder:** the snap-back exposure survives on the two inline reshape paths
  (`addFromItem:454`, `toggleRecordLock:377`) that bypass the editing deferral (see NEW-3.6).
- **Schema M2:** load path still accepts unbounded `landscape` arrays (`ssatmoenvasset.cpp:1679-1690`);
  oversize-save failure still log-only with no floater feedback (`ssatmoenvmanager.cpp:1133-1139`).
- **Schema M3:** `SS_ATMOENV_VERSION` still 1 — older builds silently drop landscape data on any save.
- **Runtime m1:** a face textured with the standard default texture `89556747…` still serializes a
  full block (`LLTextureEntry()` default-id is null; `ssatmolandscapeobject.cpp:256`).
- **Runtime m2:** no `FLAGS_PHANTOM` — scenery is solid to the client-side avatar physics.
- **Runtime m3:** no 404/failed-state surface; a 404 proxy's faces are still live capture data once
  the guard passes (num > 0, applied ≥ 0) — the wipe is fixed, the proxy-state flag is not.
- **Runtime m4:** `mAlphaMode` serialized but never applied (dead field); `invalidate()` still never
  called (reorder API lure); `toggleRecordLock` region-null fallback still truncates globals to
  region-frame floats; `setScale → addToMap` still dots scenery on the minimap.
- **Surgery minor:** `fsfloaterimport` ObjectName/ObjectDescription/ObjectPermissions sends still
  ungated (:860-947, defensive); floater min-height still clips the landscape tab's lower buttons.
- **Surgery F-1 tradeoff (accepted):** scenery still far-clips at DrawDistance — "renders like the
  sky" remains unmet by design choice.
- **Cap premise (schema M1 remnant):** the 40-face "worst case" behind the 8/12 numbers is nowhere
  enforced; a single record with hundreds of authored faces (dense mesh, or a hand-built notecard)
  can exceed the 64 KiB ceiling regardless of the count caps.

## Verified-correct highlights (re-checked, still true)

- Seed call sites (llselectmgr.cpp:1090/1140) and the `mPermissions->init(record->mCreator,
  gAgentID, record->mLastOwner, null)` ordering — correct for this tree's 4-arg `init`.
- `fromLLSD` length guards on every array (ssatmoenvasset.cpp:1495/1505/1570/1578/1596) and the
  `r.size() == 4` quaternion guard (:1586) — intact.
- LOD scale path can no longer silently go dead: the virtual hook is dispatch-active and bit-neutral
  for stock volumes (1.0 default).
- All gates verified by grep coverage: lltoolgrab :604/:740/:913/:1208/:1257, llselectmgr
  :521/:992/:1045/:6024/:6041/:6189/:6217/:9238/:9275, fscommon :214, fsfloaterimport :1156 — the
  only ungated server-send path touching scenery is the llviewermenu derez/Take family (residual
  M-3 above).