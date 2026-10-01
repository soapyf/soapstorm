# Review: Design 3 (author real, render local)

Adversarial review of `design_3_author_real_render_local.md`. Every code claim below was checked
against this tree, not against the brief. The design's core shape — author on a real rezzed
object, render from an adopted client-only `LLVOVolume` — survives attack; roughly a third of
its specific mechanisms do not.

## Verification of codebase claims

| Claim | Verdict | Correction / note |
|---|---|---|
| `adoptViewerObject` llviewerobjectlist.cpp:2246, header note :71-73 | CONFIRMED | Exact; wrapped in `<SS:Nexii>`, does the createObjectViewer bookkeeping. |
| SSWater pattern: news + adopt + `gPipeline.createObject` (sswater.cpp:313-324) | CONFIRMED | adopt at :314, createObject at :324, fresh `id.generate()` at :313. |
| `mbCanSelect` llviewerobject.h:818; water :59 / sky :437 / particles :99 set it false | CONFIRMED | All three line-exact. |
| `LLVOVolume::lineSegmentIntersect` declines on `!mbCanSelect` at llvovolume.cpp:4966 | CONFIRMED (code), IMPRECISE (mechanism) | The check exists and is first — but it is irrelevant for picking: `LLPipeline::lineSegmentIntersectInWorld` only visits a **hardcoded partition whitelist** (VOLUME, BRIDGE, AVATAR, CONTROL_AV, TERRAIN, TREE, GRASS — pipeline.cpp:7352-7358). A `PARTITION_LANDSCAPE` object is never raycast at all. Conclusion (unpickable) holds via a different gate than cited. |
| `sendSelect` llselectmgr.cpp:5524, `sendMultipleUpdate` :4892, `sendTEUpdate` llviewerobject.cpp:5528 | CONFIRMED | Line-exact; unreachable for non-selectable objects, as claimed. |
| RezObject compose lltooldraganddrop.cpp:1971-2028; `FSCommon::getGroupForRezzing()` fscommon.h:97; `RezSelected` field | CONFIRMED | Legacy message, `sendReliable(region host)` at :2028; not a cap. |
| `pack_permissions_slam` is file-local, ~25 lines, must be duplicated | **WRONG** | It is **declared in lltooldraganddrop.h:321** (llattachmentsmgr.cpp:35 includes the header for exactly this) and is ~10 lines (lltooldraganddrop.cpp:3575-3586). sslandscapeeditor.cpp should call it; duplication is unnecessary and the stated cost is wrong. |
| `allowAgentBuild()` llviewerparcelmgr.h:180 = the build-tools predicate (lltoolmgr.cpp:338) | CONFIRMED | But as a *rez-rights* gate it is the wrong predicate — see Major 6. Note the implementation is the FS-modified variant (llviewerparcelmgr.cpp:724-748: `allowModifyBy(...) || hasPowerInGroup(..., GP_LAND_ALLOW_CREATE)`). |
| "The server's rejection of a rez is silent" | CONFIRMED | No structured failure path exists anywhere in the viewer (no RezObject error alert/notification; only `RezFailTooManyRequests`/`RezFailureTookTooLong`, referenced from no .cpp). The 10 s timeout is the right shape. |
| `updateFlags` llviewerobject.cpp:7474; FLAGS_PHANTOM/FLAGS_TEMPORARY_ON_REZ object_flags.h:41/67 | CONFIRMED | object_flags.h is in indra/llprimitive, not newview. "One ObjectFlagUpdate" is optimistic: two `setFlags` calls send two messages; use `setFlagsWithoutUpdate` + one `updateFlags()`. |
| Object arrives selected via `RezSelected`; session grabs it from `getSelection()` | **WRONG (unconditional claim)** | Arrival auto-select happens in llviewerobjectlist.cpp:365-397 **only if the current tool is not `LLToolPie`** — i.e. the author must already be in build mode. With the environment floater open and default cursor mode active, the object arrives *selected on the wire* but is never added to `LLSelectMgr`, so the grab fails and the 10 s poll would misreport a successful rez as a failure. Also on this path Firestorm may apply default build prefs (`FSCommon::sObjectAddMsg`, llviewerobjectlist.cpp:381-386) — dropObject zeroes the counter (lltooldraganddrop.cpp:2032); the duplicated compose must too. |
| Capture accessors `getScaleS/getOffsetS/getRotation/getGlow/getBumpShinyFullbright/getMediaTexGen` (lltextureentry.h:142-163), `getMaterialID().asUUID()` | CONFIRMED | Files are in indra/llprimitive. `asUUID()` is at llmaterialid.h:64, not :163. Round-trip setters exist (`setBumpShinyFullbright`, :129). |
| `getRenderMaterialID(te)` llviewerobject.h:206 | CONFIRMED | Line-exact. |
| "TE capture … therefore lossless by construction" | **IMPRECISE / overclaim** | The accessors are raw; the *record* is not complete. Omitted: spotlight aim (`setSpotLightParams`, set from the build floater at llpanelvolume.cpp:1321), object extra params generally (flexible = `LLFlexibleObjectData`, llprimitive.h:274), per-face media entries beyond the texgen byte, per-TE GLTF material overrides (`setTEGLTFMaterialOverride`, llvovolume.h:225), and the light colour space (linear vs sRGB accessors both exist, llvovolume.h:262-300 — which one the record stores is unspecified). |
| Name/desc from `LLSelectNode::mName`/`mDescription` | CONFIRMED | Public members, consumed elsewhere (e.g. fsfloaterexport.cpp:545-546). |
| "Creator/last-owner … from the item because object properties only ever carry them secondhand" | **WRONG (as stated)** | ObjectProperties carries `creator_id`/`last_owner_id` directly (parsed at fsareasearch.cpp:889/903 and in llselectmgr's processObjectProperties). The item-provenance choice is justified by R2 alone; drop the false protocol rationale. |
| `setVolume(params, 0)` → `gMeshRepo.loadMesh` llvovolume.cpp:1320 | CONFIRMED | Signature llvovolume.h:236 (`detail` 0). |
| `notifyMeshLoaded()` llvovolume.cpp:1453 fires; "the override applies the record's TEs" | **WRONG** | `notifyMeshLoaded` is **not virtual** (llvovolume.h:398; dispatched non-virtually from llmeshrepository.cpp:5000). The design's hook does not exist; making it virtual is a missing 7th stock diff, or the hook must move (e.g. on `mSculptChanged`/regen). |
| `setRegion` llviewerobject.cpp:7417; created-list guard `local_id > 0` llviewerregion.cpp:3061 | CONFIRMED | Re-binning into the new region's partition genuinely works: `LLDrawable::getSpatialPartition()` resolves via `gPipeline.getSpatialPartition(mVObjp)` (lldrawable.cpp:1208-1218) against the *current* region, and `LLSpatialPartition::move` removes from the old partition's group first (llspatialpartition.cpp:1004-1008). |
| `killObject` llviewerobjectlist.cpp:1598 markDead only | CONFIRMED | |
| `calcLOD` llvovolume.cpp:1664, not virtual (llvovolume.h:433); `updateLOD` virtual :247; stretch-the-distance trick valid | CONFIRMED | Base formula elements all present (sDistanceFactor, near-boost, FOV zoom, sLODFactor, llvovolume.cpp:1761-1782). Only caller is `updateLOD` (:1849), so the mutate/restore is safe. |
| `mInfiniteFarClip` llspatialpartition.h:436 | CONFIRMED | Default false (:937), honoured at :1469; precedents set it in partition ctors (llvowater.cpp:298, llvosurfacepatch.cpp:984). |
| `getSpatialPartition` virtual dispatch pipeline.cpp:7626-7637; culling/render general | CONFIRMED | `updateCull` iterates all partitions (pipeline.cpp:2692-2702), so main render **and shadow culls** (which reuse `updateCull`, pipeline.cpp:10755) include a new partition; `LLVolumePartition` inherits `LLVolumeGeometryManager` (llspatialpartition.h:697), so faces/shadows/light geometry come free. |
| Partition injection: enum after PARTITION_VOLUME (llviewerregion.h:99), push_back "beside the water partitions at 724-725" | **WRONG as specified** | The vector is index-aligned with the enum (llviewerregion.cpp:722-735). Enum position 8 requires the push **after `LLVolumePartition` (line 729)**, not beside water (index 3). As written, every partition from TREE onward resolves to the wrong class. |
| `getIsFullPerm` llviewerinventory.cpp:2482; `DAD_MESH` llviewerassettype.cpp:81; ssfloatertexturelist.cpp:175 | CONFIRMED | All line-exact. |
| `llvolumemessage.cpp:463-480` curve accessors | CONFIRMED | `constrainVolumeParams`; file is indra/llprimitive. |
| Manager integration: `editable()` (ssatmoenvmanager.h:59), `isModified` via llsd_equals (~:61), `toPrettyXML` :739, `mPrecipitationTypes` precedent | CONFIRMED | `llsd_equals` at indra/llcommon/llsdutil.h:154. |
| Applier track resolution ssatmoenvapplier.cpp:176-188; `want_active` gate | CONFIRMED | want_active at :146; `activeTrackIndex()` is genuinely new. |
| `SSWaterWorld::update()` at llviewerdisplay.cpp:1000 | CONFIRMED | |
| Mesh-preview precedent llmodelpreview.cpp:3911-3922 | CONFIRMED | |
| "pcode `LL_VO_VOLUME` retained" | **WRONG (name)** | The constant is `LL_PCODE_VOLUME` (factory switch in `LLViewerObject::createObject`, → `new LLVOVolume`). No `LL_VO_VOLUME` exists. The *claim* (factory can't build the subclass) is correct. |
| Reflections included in the inherited partition | PARTIAL | Probe cube renders cull generically, so scenery *appears in* reflections — but reflection-probe auto-registration is `PARTITION_VOLUME`-only (llreflectionmapmanager.cpp:669, wired from llspatialpartition.cpp:609). Landscape groups cannot seed probes. |
| `SS_ATMOENV_MAX_TRACKS = 8` | CONFIRMED | ssatmoenvasset.h:44. |

## Fatal / major findings

1. **Re-authoring from a record has no mechanism.** `RezObject` rezzes an *inventory item*
   (InventoryData block, lltooldraganddrop.cpp:2025-2026); a record is not an item. §4's
   "re-rezzes a fresh temp object … wearing the record's state" therefore requires either
   (a) re-finding the original mesh item in inventory — which may be deleted (the record lives
   in the env asset, the item does not have to survive), ambiguous with duplicate copies, and
   is nowhere specified — and then (b) seeding the record's TEs/transform/materials onto the
   object via viewer-sent updates (ObjectSelect + `sendMultipleUpdate` + movement) — i.e.
   exactly the selection-manager write path the design brags is "unreachable by construction"
   and absent. The one mode the design's whole editor-fidelity argument rests on for round two
   is hand-waved.

2. **The arrival-selection premise is conditional, and the design doesn't know it.** With the
   pie tool active (author simply has the floater open, not build mode), a create-selected
   object is *not* auto-selected (llviewerobjectlist.cpp:365-397). The session's grab, the
   property-name capture (ObjectProperties only flows after a real ObjectSelect), and the 10 s
   success poll all silently break in the most common case. The session must explicitly
   `selectObjectAndFamily` (or force the build tool) on arrival — unstated, and it is the
   difference between working and not.

3. **The partition wiring, as specified, misaligns every partition in the region.** Enum
   insert after `PARTITION_VOLUME` (line 99) but push_back beside the water partitions (724-725)
   shifts TREE/PARTICLE/GRASS/VOLUME/BRIDGE/AVATAR/CONTROL_AV/HUD_PARTICLE/VO_CACHE/NONE by one.
   Trees would route into the landscape octree, avatars into bridges, and the VO cache would
   vanish. Trivial to fix (insert the push after line 729), but the design names the wrong
   lines and a builder following it literally ships a catastrophic bug.

4. **The stated hydration hook does not exist.** `notifyMeshLoaded` is non-virtual
   (llvovolume.h:398). Either llvovolume.h gains a second tagged one-word change (missing from
   the §11 diff list) or the design applies TEs from a different signal. Also note the
   transform is deferred to mesh arrival in §2, so the object sits at the region origin in the
   partition until the mesh lands — apply the transform at hydrate time; nothing about
   position needs the face count.

5. **"Lossless by construction" is false at the record level.** Beyond the non-material TEs the
   capture *does* take, the authored look includes: spotlight aim params (a build-floater
   control, llpanelvolume.cpp:1321), flexible/extra-parameter state (`LLFlexibleObjectData`),
   per-face media entries beyond the texgen byte, GLTF material overrides, and unspecified
   light colour space. A spot-lit scenery piece or a wind-blown banner captures *wrong*, and the
   runtime then renders "exactly a rezzed mesh" of something the author never rezzed. The claim
   needs to be scoped to "TEs + transform + light core" or the schema needs an extra-params bag.

6. **`allowAgentBuild()` is the wrong rights predicate for rez.** It tests modify rights
   (`allowModifyBy` / group create power — itself the FS-modified variant, llviewerparcelmgr.cpp:724-748),
   not parcel *create-object* rights. False positive: modify-but-not-create land → rez sent →
   server silently refuses → 10 s timeout → pointless wait. False negative: anyone-can-create
   parcels where the agent lacks modify → the design enters proxy mode although the server would
   happily rez. The proxy fallback itself is coherent (mesh id + metadata straight from the
   item, client-only render, spinner positioning), but it will trigger when it shouldn't and
   the honest fix is testing create rights, not the build-tool gate.

7. **R4's UUID-only matching can adopt the wrong render state.** Reuse deliberately skips
   `setVolume` because "matching by UUID guarantees the asset is the same" — but the record
   stores full `LLVolumeParams` precisely because same-mesh records can differ (sculpt-type
   variants; and the same argument the design itself makes for VolumeParams vs the item at
   capture). A reused object keeps its old params and TEs are then reapplied on top. Compare a
   params hash (or always `setVolume`) and say which coordinate frame the "nearest" comparison
   uses (live objects are global; incoming records are region-local until re-anchored) — the
   ordering of re-anchor vs track-diff inside `update()` is undefined.

8. **Two of the design's own stock-file facts contradict each other** (see Major 3) **and the
   diff list is short**: llvovolume.h needs `notifyMeshLoaded` made virtual (or the hook
   redesigned); the duplicated compose must replicate `FSCommon::sObjectAddMsg = 0;`
   (lltooldraganddrop.cpp:2030-2032) or Firestorm's default build preferences can stomp the
   fresh object's materials before capture (llviewerobjectlist.cpp:381-386).

## Minor findings

- **Picking**: the design's mbCanSelect mechanism is dead code for picks — the whitelist
  (pipeline.cpp:7352-7358) never visits the partition. Keep `mbCanSelect = false` anyway (hover
  paths and `LLVOPartGroup`-style consistency), but cite the whitelist; it also means the
  shoulder-camera world-geometry raycast (pipeline.cpp:7503-7512) ignores scenery by default —
  desirable here.
- **Reflection probes**: cube renders include the scenery, but probe auto-registration is
  VOLUME-partition-only (llreflectionmapmanager.cpp:669). Fine in practice; do not claim full
  parity.
- **Texture LOD is not stretched**, only mesh LOD: at 2048 m the pixel area drives discard
  level, so distant scenery renders soft. Either accept and say so, or plan a texture boost on
  the faces.
- **Temp objects skip the region's VO cache** (llviewerobjectlist.cpp:619) — consistent with
  self-cleaning, worth knowing.
- **Derez-to-trash litters the author's trash** with one mesh object item per capture round;
  the design says nothing about it.
- **`killObjects(regionp)`** (llviewerobjectlist.cpp:1648-1660) kills by `mRegionp`: if a
  departed region is destroyed before the re-anchor runs, runtime objects die until the
  next signature rebuild — a one-frame pop the SSWater per-region rebuild never has.
- **Free-mode records are estate-pinned**: global coordinates are meaningless on another
  estate; the object renders in empty space. Spec-compliant (R6), but say it out loud.
- **Two `setFlags` calls = two ObjectFlagUpdate messages**; batch via
  `setFlagsWithoutUpdate` + one `updateFlags()`.
- **`LL_VO_VOLUME` does not exist** — it's `LL_PCODE_VOLUME`.
- **Camera-frame rez ray**: with a free camera far from the avatar, the rez point may be out
  of range → silent no-arrival → 10 s proxy fallback for something the author thinks worked.
  Anchor the ray to the avatar, not the camera.

## Silently undefined behaviour

- **What bumps the rebuild signature's "asset revision counter" when capture writes into
  `editable()`** — the plumbing from `SSLandAuthoringSession::capture()` to
  `SSLandscapeWorld::update()` is asserted, not specified. Without it, saved records never
  hydrate until some unrelated signature member moves.
- **Per-record suppression identity**: what binds "the record under an open session" when the
  slot is deleted and re-inserted at the end of the list (a stated flow) — node identity? slot
  index? The load-bearing rule (the design's own words) has no definition.
- **Track crossing while the session's record belongs to the outgoing track** — suppression is
  keyed to the active track's hydration; crossing away mid-session is fine, crossing back must
  re-suppress. Unstated.
- **A second drop while a session is live** ("one drop = one session" — then what? reject?
  capture-and-switch?).
- **Capture racing property arrival**: `RezSelected` rez → selection → ObjectProperties takes a
  round trip; a mid-session capture immediately after arrival reads an empty name (the item-name
  fallback exists, but the close-time capture may then silently rewrite `name` differently).
- **Rez range**: no statement of where the 5 m camera ray is allowed to land relative to the
  avatar, or what the server's distance limit does to the 10 s poll.
- **Proxy-record TE state**: hydration of a rights-denied record — default TEs, or something
  else? (Reasonable either way; undefined.)

## Steal-worthy parts

- **The authoring loop itself**: rez → stock build tools → capture → derez is the cheapest
  path to a *complete* editor (undo, PBR, light, name/desc for free) and keeps llselectmgr and
  the tool dictionary at zero diffs. This is the strongest idea in the five-design set for
  editing UX, and the design is admirably explicit about where its snapshot semantics bite.
- **`pack_permissions_slam` is already exported** (lltooldraganddrop.h:321) — the correct
  implementation is one `#include` and zero duplication; llattachmentsmgr.cpp:35 is the
  precedent to cite.
- **The calcLOD distance-stretch** (mutate `mDistanceWRTCamera`, call base, restore) — verified
  safe: the only reader is inside the same call chain, and every stock factor (sLODFactor,
  near-boost, FOV zoom) flows through untouched. One-word virtualization is exactly the kind of
  stock diff this fork should make.
- **`LLLandscapePartition : LLVolumePartition` with `mInfiniteFarClip = true`** — fully
  precedented (water, terrain patches), inherits the whole geometry manager, and integrates with
  shadow culls for free because `updateCull` is partition-generic.
- **The pick-whitelist inversion**: a custom partition type is *invisible to raycasts by
  default* (pipeline.cpp:7352) — unpickability without any per-object gate, which is more robust
  than the design's own stated mechanism.
- **Capture-on-close diffing with `llsd_equals`** against the manager baseline — event-driven
  persistence that reuses `isModified()` verbatim is the right shape for R7.

## Verdict

**Feasibility: 6/10.** The premise is sound and mostly precedented, but the design's load-bearing
mechanisms have concrete defects that a builder following the text would hit one by one.

**Three worst problems:**
1. Re-authoring from a record is unmechanised — RezObject needs an inventory item, and seeding
   record state needs the very send paths the design claims to avoid (Major 1).
2. Arrival selection is conditional on the active tool; grab, name capture and the failure poll
   all break in default cursor mode (Major 2).
3. The partition wiring as specified misaligns every partition after index 3, and the stated
   hydration hook (`notifyMeshLoaded` override) doesn't exist — both are one-line fixes the
   design currently gets wrong (Majors 3-4).

**Three best ideas:**
1. Real-object authoring with capture-derez: total editor fidelity for near-zero core diff.
2. Client-only `LLVOVolume` in a dedicated infinite-far-clip partition: stock pipeline
   wholesale, with raycast-invisibility falling out of the pick whitelist for free.
3. The calcLOD input-stretch override: R3 satisfied with one word in llvovolume.h and a
   five-line override.
