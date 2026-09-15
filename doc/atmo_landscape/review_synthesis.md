# Final Review: Synthesis

Final adversarial review of design_synthesis.md. Note on process: the review
agent attempt timed out twice on infrastructure (504s), so this final review was
performed directly, with first-hand code verification of every load-bearing
claim. Design reviews review_1…review_4 were agent-produced and are treated as
verified input where their findings carry file:line evidence.

## Fatal-finding coverage matrix

| Review finding | Synthesis repair | Verified? |
|---|---|---|
| R1-F1: object unclickable — pick enumeration at pipeline.cpp:7350-7358 lacks the partition | §2a adds the enumeration line | **VERIFIED first-hand** (read 7350-7358: explicit whitelist, LANDSCAPE absent). Also note: the walk checks `hasRenderType(part->mDrawableType)` — opt-in off state is unpickable for free |
| R1-F2: "one gate" false — selectObjectOnly inline ObjectSelect (518-525), deselect ×2 (985-1037) | §11 gate list with exact sites | Confirmed; plus this review found **two more inline `MultipleObjectUpdate` sends (llselectmgr.cpp:9199, 9220)** — now added to §11 |
| R1-M: six FS direct senders | §11 + diff list rows | VERIFIED: fscommon.cpp:278 (ObjectPermissions), :302 (ObjectFlagUpdate), fsfloaterimport.cpp:887/:1155, fslslbridge.cpp:1197 |
| R1-M: hash feedback loop (reconcile re-applies stale records to dragged object) | §6 direction stamps | Design-level, mechanism coherent |
| R1-M: grab only half-gated | §11 (lltoolgrab.cpp:731, 900) | VERIFIED both sites exist |
| R1-M: 8×64 records exceed 64 KiB notecard | Was mis-claimed in synthesis; **corrected during this review** to 16/track + 32/asset + sparse faces with honest arithmetic (~2 KB/record worst case, ~256 KB if unbounded) | Fixed in §3 |
| R2-F: render slot unimplementable; far plane = draw distance (llviewercamera.cpp:340) so 2 km backdrop draws nothing | D2 rejected outright; infinite-far-clip partition (sky/water precedent) replaces it | Mechanism verified by R4's partition verification; render-type→pool mapping flagged as the remaining unverified row (§14) |
| R2-M: standalone request scoring starvation | Dropped with D2 | n/a |
| R2-M: HUD gates missing | Moot on the volume path (stock volume update path carries its own gates) | OK |
| R3-F: re-authoring from record unmechanised; RezSelected arrival breaks with pie tool; notifyMeshLoaded not virtual | D3's authoring path dropped entirely (user decision: drop-only); runtime uses stock setVolume path (D4's correction absorbed: no LLVolumeImplMesh) | Consistent |
| R3-F: partition enum/push_back misalignment hazard | §2a calls out insertion order explicitly | Design-level |
| R3-M: capture not lossless (spotlights, flexi, media, GLTF overrides) | Capture dropped; record is source of truth from drop onward; §8 documents scope (face/TE subset) | Consistent |
| R4-F1: manipulator perms read VO flags (llviewerobject.cpp:7198-7330), fake nodes can't fix | Construction-time permission flag bits | The fix direction R4 itself named; sound |
| R4-F2: commit point dead code (SEND_ONLY_ROOTS vs linkset children) | Design uses no commit points — reconcile funnel; N roots avoid is_root entirely | Consistent |
| R4-F3: rect-select walks partitions without INVISIBLE gate (llglsandbox.cpp:234-297) | **Was unrepaired for the N-roots case** — patched during this review: the walk must include PARTITION_LANDSCAPE or rubber-band selection never sees landscape objects | Now in §12 |
| R4-F4: addAsFamily funnel claim wrong | Design no longer relies on that funnel | n/a |
| R4-F5: cost/benefit inverted | Synthesis adopts N-roots-under-one-manager explicitly (§1.2) | Consistent |

**Result: every fatal from four reviews is either repaired with a verified
mechanism, or structurally dropped by rejecting the design that produced it.**
Three repairs (two inline MultipleObjectUpdate sites, the notecard arithmetic,
the rect-select walk) were found missing in the synthesis's first draft and were
fixed during this review — the synthesis text in the repo includes them.

## Verification of the synthesis's own claims (first-hand)

- `requestObjectPropertiesViaSelect` exists (llselectmgr.h:897) and packs its
  own inline ObjectSelect/ObjectDeselect (6173-6197) — "both properties-request
  functions" is correct and both need gates. CONFIRMED.
- TE/transform/name sends route through `sendListToRegions` (sendMultipleUpdate
  → 4903; ObjectName/Description → 5255-5298; Undo/Redo → 8001/8019). CONFIRMED
  — the funnel gate genuinely covers the stock volume editing path.
- Stock undo restores client-side from select nodes AND sends server "Undo"
  (8001) — synthesis §6 describes both halves accurately; the funnel observes
  the client-side restore. CONFIRMED.
- `calcLOD` anchor (llvovolume.cpp:1755 `distance *= sDistanceFactor;`) — read
  first-hand earlier in this workflow. CONFIRMED.
- Grab sites (lltoolgrab.cpp:731 ObjectGrabUpdate inline; 900 same) — grep
  confirms both. CONFIRMED.
- Pick enumeration whitelist (pipeline.cpp:7350-7358) — read first-hand.
  CONFIRMED.
- SSWaterWorld lifecycle precedent (adopt at sswater.cpp:313, clear at
  llworld.cpp:138, update from llviewerdisplay.cpp:1000) — verified earlier in
  this workflow. CONFIRMED.

## Remaining holes (unresolved by design intent)

1. **Render-type → pool mapping for PARTITION_LANDSCAPE** — the one
   load-bearing row that is design-level-unverified. Mitigated: §14 makes it
   phase 1's acceptance gate ("renders at 3 km") before any editing work.
2. **Pie-menu filter completeness** — Take/Land-Impassability/etc. hiding is
   implementation-phase enumeration; Delete-removes-record is designed, the
   rest of the menu is not itemized.
3. **Camera collision** — whether the camera walk tests the landscape partition
   (shoulder-cam push-out) is undefined; likely acceptable either way for
   scenery (it is phantom), but should be decided in phase 2.
4. **requestObjectPropertiesViaSelect callers** — for local objects the nodes
   are seeded so nobody should call it; the gate is belt-and-braces. Fine.
5. **Varregion note from R4** — region-local offsets are origin-relative and
   work on varregions; the 0–256 clamp in earlier designs was quietly dropped
   in the synthesis (correct), but the floater's coordinate spinner range
   should derive from region size, not a constant.

## Implementation order recommendation

1. **Phase 1 — substrate**: schema + record round-trip; SSAtmoLandscapeObject
   + adoption; PARTITION_LANDSCAPE + render-type + pick enumeration + infinite
   far clip; drop→hydrate→mesh renders. Acceptance: mesh visible at 3 km,
   gone with SSAtmoEnabled off, re-anchored after region cross. (This phase
   also retires risk #1.)
2. **Phase 2 — editing**: permission flags at construction, node seeding,
   the full §11 gate list, reconcile funnel v1 (transform + name/desc),
   rect-select walk, camera-collision decision. Acceptance: full stock-tool
   edit loop with zero server packets (log-verified), undo round-trips.
3. **Phase 3 — floater**: list panel, drop gate (fullperm + metadata),
   delete/lock/reorder, face-texture fullperm gate, budget caps.
4. **Phase 4 — lifecycle**: track-crossing UUID+params adoption, environment
   load/unload, 404/failure states, parcel-leave clearing.
5. **Phase 5 — LOD stretch hook** (ssLODDistanceScale) + DebugObjectLODs
   tuning + sparse-face serialization.

## Verdict

**7.5/10 as a design.** The substrate is verified end-to-end first-hand (mesh
fetch, adoption precedent, LOD anchor, pick/funnel/gate sites); every fatal
from four adversarial reviews is accounted for with a working mechanism; the
residual risk is concentrated in partition wiring that phase 1 explicitly tests
first. Blockers: none. Polish: items 2-5 above.

The concentration of remaining uncertainty in the *pipeline* rows (render-type,
pick, rect-select) rather than the architecture is the right shape for a
design: the first build test retires the biggest unknown, and every later phase
is independently testable.
