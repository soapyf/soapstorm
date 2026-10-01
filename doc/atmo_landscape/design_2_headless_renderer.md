# Atmo Magic Landscape — Design 2: the headless renderer

The landscape is not a viewer object. There is no `LLViewerObject`, no drawable, no octree node,
no spatial partition, no selection manager, and therefore no send-gating problem anywhere in this
design — nothing here can ever be selected, moved by a build tool, or sent to a simulator, because
nothing here is a thing the server knows about. The whole feature is one renderer singleton that
fetches mesh assets by UUID, builds its own `LLVertexBuffers` from the decoded faces, and draws
them in its own pass in the post-deferred pool walk. Every requirement R1–R7 is satisfied by
schema, arithmetic and a diff loop — not by objects.

This is the same bet Atmo Magic already made twice and won: the celestial billboards are drawn as
quads straight into the sky pass from the applier's published list (lldrawpoolwlsky.cpp:766-824),
and the SS weather passes draw straight into the post-deferred pool walk from their own singletons
(pipeline.cpp:4463-4466). A mesh mountain is the same idea with real geometry. The standalone
mesh-rendering precedent is the upload preview: `LLFloaterModelPreview` builds vertex buffers from
decoded faces and draws them with its own shader, no region object, no pipeline registration
(llmodelpreview.cpp:3911-3922).

## 1. Architecture

### The classes

| Class | File | Role |
|---|---|---|
| `SSAtmoLandscapeRenderer` | `ssatmolandscape.h/cpp` (new) | Singleton. Ticks once per frame from the existing Atmo block in `llviewerdisplay.cpp`, right after `SSWaterWorld::update()` — reads the same frame's resolved track the way `SSWaterWorld` reads the applier's state. Owns the live instance list, the mesh cache, the LOD rule, the region-anchor math and the render pass. |
| `SSAtmoLandscapeMesh` | `ssatmolandscape.h/cpp` | One per asset UUID, refcounted by instances. Holds the decoded `LLVolume` per requested LOD (`LLPointer<LLVolume>`) and the built GPU buffers keyed by material slot. Implements `SSAtmoMeshFetchListener`. |
| `SSAtmoLandscapeInstance` | `ssatmolandscape.h/cpp` | One live scenery object: an index into the mesh cache, the adopted `SSAtmoEnvLandscape` record, a world-transform cache, and the per-frame frustum result. No base class — a plain struct the renderer owns by value in a vector. |
| `SSAtmoMeshFetchListener` | `ssatmolandscape.h` | Two-method interface the mesh repository extension calls back on the main thread: `onMeshReady(uuid, lod, LLVolume*)` and `onMeshFailed(uuid, lod)`. |
| `SSPanelLandscape` | `sspanellandscape.h/cpp` (new) | The editor tab panel inside `SSFloaterAtmoEnv` (see §4). |

New shader files: `app_settings/shaders/class1/deferred/ssLandscapeV.glsl` and
`ssLandscapeF.glsl`, bound through one `LLGLSLShader gSSLandscapeProgram` registered in
`llviewershadermgr` exactly the way `gSSCelestialProgram` is (declaration, `mShaderList` push,
unload call, load block at llviewershadermgr.cpp:2089-2101). One program serves opaque, cutout and
blended faces through an `ss_alpha_mode` uniform (0 = opaque, 1 = cutout at 0.5, 2 = blended), so
there is one shader to maintain and one to break.

### What deliberately does not exist

- **No pcode, no factory entry, no partition.** The renderer is not registered with `gPipeline`
  as a pool; it borrows the frame in a single injected block (§11, pipeline.cpp). Nothing in
  `LLSelectMgr`, `gObjectList`, the octree or the interest list ever sees a landscape object.
- **No `LLViewerObject::setRegion` call** (llviewerobject.cpp:7417). The brief cites it as the
  re-anchoring concept; the headless design replaces the call with arithmetic (§2, region
  crossing) because there is no object to move — one instance, repositioned per frame from the
  agent region's origin, exactly the way `SSWater`'s tile ring is anchored to the agent region.
- **No stock build tools.** Editing is the floater only (§4). The entire class of
  `LLSelectMgr::sendSelect`/`sendMultipleUpdate` gating problems (llselectmgr.cpp:5524, 4892)
  is dissolved by construction rather than solved.

### The mesh repository extension (the one place we touch stock plumbing)

`LLMeshRepository`'s delivery is volume-coupled: `loadMesh(LLVOVolume*, params, lod)`
(llmeshrepository.cpp:4482) registers a waiting volume in `mLoadingMeshes`, and
`notifyMeshLoaded` (4963) drops the decoded volume on the floor when nobody is waiting. The repo
thread itself is already UUID-keyed end to end — `mMeshHeader` is a
`std::unordered_map<LLUUID, LLMeshHeader>`, and `PendingRequestUUID`
(llmeshrepository.h:273) already carries skin/decomposition/physics requests by bare UUID. The
extension is one request type, two entry points, and two delivery branches:

- `EMeshRequestType` gains `MESH_REQUEST_LOD_STANDALONE` (llmeshrepository.h:66-74). Served by
  `PendingRequestUUID(mesh_id, MESH_REQUEST_LOD_STANDALONE)` — no new request class.
- `LLMeshRepository::loadMeshStandalone(const LLUUID& mesh_id, S32 lod,
  SSAtmoMeshFetchListener* listener)` — main-thread entry. Builds a synthetic
  `LLVolumeParams` (sculpt type `SCULPT_TYPE_MESH`, sculpt ID = mesh_id), records the listener in
  a new `mStandaloneLoads` map keyed by UUID, and queues the request. The thread side is a thin
  public wrapper `LLMeshRepoThread::loadMeshLODStandalone(mesh_id, lod)` that constructs the same
  synthetic params and calls the existing private
  `loadMeshLOD(mesh_id, mesh_params, lod)` (llmeshrepository.cpp:1362) — header routing, byte
  ranges, the disk cache, `constructUrl` and decode (`lodReceived`, 2481) are all reused
  untouched, because none of them need a requesting volume.
- `LLMeshRepository::cancelMeshStandalone(mesh_id, listener)` — removes the listener; the
  in-flight HTTP request is deliberately left to complete (the header stays cached either way).
- Delivery: `notifyMeshLoaded` and `notifyMeshUnavailable` gain a tagged branch **before** the
  volume-coupled lookup: if `mStandaloneLoads` holds this mesh/lod, hand the decoded `LLVolume`
  (an `LLPointer`, refcounted) to each listener via `onMeshReady`/`onMeshFailed` and erase. The
  volume never reaches the system volume manager and no `LLVOVolume::notifyMeshLoaded` runs —
  nothing stock changes behaviour. The `notifyLoadedMeshes` dispatch switch (4855-4874) gains the
  corresponding `case MESH_REQUEST_LOD_STANDALONE`.

## 2. Data flow

**Drop → record.** The editor's mesh drop target accepts `DAD_MESH` (llviewerassettype.cpp:81
maps `AT_MESH` → `DAD_MESH`). The item must pass `LLViewerInventoryItem::getIsFullPerm()`
(llviewerinventory.cpp:2482 — modify-ok & copy-ok & transfer-ok); the asset UUID is taken from
`getProtectedAssetUUID()`, which is null for anything short of full-perm, so the gate and the
payload are one call. Name, description, creator and last owner are captured from the item's
permissions at drop time (R2) and become read-only fields of a new `SSAtmoEnvLandscape` record
in `SSAtmoEnvManager::editable().mTracks[selected].mLandscape`. The renderer's next frame diff
sees the new record and requests the mesh.

**Record → hydration → render.** `SSAtmoLandscapeRenderer::update()` runs per frame (after the
applier, same frame's state):

1. Gate: `SSAtmoEnabled` && `SSAtmoEnvManager::hasAsset()` — mirroring the applier's
   `want_active` (ssatmoenvapplier.cpp:142-155). Inactive → kill all instances, return.
2. Track resolve: the applier publishes the active track index it just computed
   (`SSAtmoEnvApplier::lastPrimaryTrack()`, a one-line publish on the existing
   `SSAtmoEnvTrackResolver::resolve` call at ssatmoenvapplier.cpp:176). The renderer reads that
   index — it never re-runs the resolver, so renderer and sky can never disagree about which
   track is live. Track index changed → run the adoption diff (below).
3. Record diff (the applier's `put()` cache-and-diff idiom, ssatmoenvapplier.cpp:545):
   per record, diff `(mesh_id, transform, face TE revision)` against the live instance; only
   changed instances rebuild transforms or texture binds. New mesh IDs →
   `loadMeshStandalone`; kills release instances.
4. Frustum-cull each instance against the camera (§7), bind `gSSLandscapeProgram`, draw.

**Track crossing (R4).** On a primary-track index change the renderer does a UUID adoption pass
over the new track's list: a record whose `mesh_id` matches a live instance keeps that instance
— the decoded volume, the built buffers and the resident textures all stay — and adopts the new
record's transform, scale, rotation and face slots (R4's "no re-download, no rebuild pop"). A
UUID only in the new track hydrates; a UUID only in the old track is killed. Presentation is an
instant cut: the same "a preset can't be interpolated" idiom that governs track-boundary swaps,
applied to geometry. Adoption is O(records) and touches no GL state when UUIDs persist.

**Region crossing (R6).** There is nothing to re-anchor — the transform is computed, not stored.
Per frame, per instance: region-locked records resolve as
`global = gAgent.getRegion()->getOriginGlobal() + (x, y, 0)`, with **z used as-is** (world
metres — the shared sea-level datum the brief names, so Z needs no region math at all); free
records use their stored global position verbatim. Teleport to an estate copy lands the mesh at
the same region-local spot automatically on the next frame; there is exactly one instance, not
per-region copies — the deliberate contrast with the `SSWater` family's per-region planes.
Editor shows region-local X/Y and world Z, so what you author is what every region gets.

**Load/unload of an environment.** On unload (`SSAtmoEnvManager::unload`) the gate in step 1
goes false and every instance is killed; buffers are released with the mesh cache. On a parcel
driving a *different* environment, the same adoption diff runs against the new asset — a mesh
UUID present in both keeps its buffers across the swap, so an estate whose regions share
scenery crosses without a pop.

## 3. Schema

`SSAtmoEnvAsset` gains, on every track (`ssatmoenvasset.h` — an SS file, not a stock diff):

```
struct SSAtmoEnvLandscape
{
    LLUUID      mMeshId;                  // the mesh asset (required)
    std::string mName;
    std::string mDescription;
    LLUUID      mCreator;                 // read-only record field (R2)
    LLUUID      mLastOwner;               // read-only record field (R2)

    bool        mRegionLocked = true;     // R6 mode flag

    LLVector3   mRegionPos;               // region-local x/y (0-256), world z — region-locked
    LLVector3d  mGlobalPos;               // used only when !mRegionLocked

    LLQuaternion mRotation;
    LLVector3   mScale = LLVector3(1,1,1);

    struct FaceSlot { S32 mIndex = -1; LLUUID mTexture; LLColor3 mColor = LLColor3(1,1,1); };
    std::vector<FaceSlot> mFaces;         // mIndex -1 = default slot for unlisted faces

    LLUUID      mMaterial;                // reserved, phase 2, round-trips unused
    F32         mRenderDistance = 0.f;    // 0 = the global default

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};
// SSAtmoEnvTrack gains:  std::vector<SSAtmoEnvLandscape> mLandscape;
```

Serialised into the notecard's per-track map (pretty XML, hand-editable, via the existing
`SSAtmoEnvTrack::asLLSD` round-trip):

```
"landscape": [
  {
    "mesh_id":      "5f3c…",
    "name":         "North Ridge",
    "description":  "distant backdrop range",
    "creator":      "uuid",
    "last_owner":   "uuid",
    "region_locked": true,
    "position":     {"x": 128.5, "y": 24.0, "z": 950.0},
    "global_position": {"x": 256128.5, "y": 256024.0, "z": 950.0},
    "rotation":     {"x": 0.0, "y": 0.0, "z": 0.7071, "w": 0.7071},
    "scale":        {"x": 4.0, "y": 4.0, "z": 4.0},
    "faces":        [
      {"index": -1, "texture": "uuid", "color": {"r": 1.0, "g": 1.0, "b": 1.0}},
      {"index": 3,  "texture": "uuid", "color": {"r": 0.8, "g": 0.7, "b": 0.6}}
    ],
    "material":        "uuid",
    "render_distance": 0
  }
]
```

Rules: `position` is present when `region_locked` is true (region-local X/Y, world Z);
`global_position` only when false (both are emitted for a hand-editable document, each mode
reads its own). Missing `landscape` on an older asset parses to an empty list — every field
defaults, the established pattern for all track blocks. An older viewer that re-saves an asset
drops the unknown key (the same fate `mPrecipitationTypes`-era documents accepted); no schema
version bump is needed or made.

## 4. Editing UX

A **Landscape tab** in the existing environment editor (`SSFloaterAtmoEnv`, panel
`SSPanelLandscape` in `floater_ss_atmo_env.xml`), per selected track. Stock build tools are not
involved anywhere, so there is no selection manager, no send-gate, no permission fetch, and no
"cannot edit because someone else owns it" — the record is client state in a notecard, and the
only permissions that matter are the asset gates at drop time.

- **List.** One row per record: name, mesh asset chip, delete button, click to select. Cap of
  `SS_ATMO_LANDSCAPE_MAX_OBJECTS = 32` records per track (R5; rationale in §9).
- **Mesh drop target.** `handleDragAndDrop` written to the `SSTextureListCtrl::handleDragAndDrop`
  pattern (ssfloatertexturelist.cpp:175). Accepts `DAD_MESH` only after the fullperm gate; a
  non-fullperm drop is refused with `ACCEPT_NO` and a tooltip ("Landscape meshes must be
  full-perm — the environment copies across an estate"). Dropping onto the tab body adds a
  record; dropping onto a selected record's mesh chip replaces its mesh (R4 adoption rules
  apply live).
- **Transform.** Position/rotation/scale spinners: X/Y spinners dial 0–256 region-local and are
  replaced by global read-outs when the **region-locked** toggle is off (the toggle is a plain
  checkbox — a property of the record, not of the moment, so it is not keyframed; the same call
  `SSAtmoEnvTrack::mWeatherSourceDeck` makes). Z is free world metres in both modes. Rotation
  in XYZ degrees, scale 0.01 floor. Live preview is free: the renderer diffs the working asset
  every frame, so an edit is on screen the same frame — no apply button.
- **Name/description.** Text fields into the record (R2 capture seeds them). Creator and last
  owner render as read-only name rows beneath.
- **Face slots.** A texture-list section following the texture-picker vocabulary: a
  "default face" row always present (slot index -1, covering every face not named), plus one row
  per decoded face once the mesh is hydrated (the renderer publishes the decoded face count on
  `SSAtmoLandscapeMesh`; until then the list shows only the default row). Each row is a chip +
  stock `LLTextureCtrl` picker, accepts `DAD_TEXTURE` through the same fullperm gate
  (`getProtectedAssetUUID` again), plus a tint colour swatch. PBR material drops are refused in
  phase 1 with a tooltip; the `material` field exists in the schema so a phase-2 viewer can fill
  it without a format break.
- **Undo.** The panel keeps a session undo stack (32 deep) of whole-record LLSD snapshots,
  pushed on every commit handler; Ctrl+Z/Ctrl+Shift+Z walk it. This is editor-local and
  deliberately simple — the durable safety net is the environment's own Save/Revert pair against
  `mBaseline`, which already covers every tab. Nothing here touches `LLUndoStack` machinery.

## 5. Persistence

Event hooks, not a reconcile loop — because the working asset is the single source of truth and
the renderer is a *reader* of it, there is nothing to reconcile. Every commit handler in
`SSPanelLandscape` writes through to
`SSAtmoEnvManager::editable().mTracks[i].mLandscape` and marks the manager modified (the
existing `isModified`/asterisk machinery picks the change up with zero new plumbing). Saving
rides `SSAtmoEnvManager::saveNotecard` unchanged; the Bridge/parcel-discovery machinery carries
the notecard across an estate unchanged (R7). On load, `SSAtmoEnvTrack::fromLLSD` parses the
list and the renderer's per-frame diff hydrates — no load-time hook exists, because the next
frame is the hook. The one deliberate property of this shape: an author editing track 3's
landscape while standing on the ground track sees nothing change in the world, exactly as with
every other per-track tab.

## 6. LOD strategy (R3)

The renderer picks the LOD; no stock LOD code runs, and `LLVOVolume::calcLOD`
(llvovolume.cpp:1664) is untouched — the "calcLOD is not virtual" trap in the brief is
sidestepped, not negotiated with. The rule, in `ssatmolandscape.cpp`:

- `distance = (camera - instance world centre).length()`, `radius = 0.5 * |scale ⊙ mesh bbox|`
  from the decoded volume's bounds.
- `tan_angle = (LLVOVolume::sLODFactor * radius * SS_LANDSCAPE_LOD_DISTANCE_STRETCH) / distance`
  with `SS_LANDSCAPE_LOD_DISTANCE_STRETCH = 8.0f`, then
  `LLVolumeLODGroup::getDetailFromTan(tan_angle)` — the stock threshold table
  (`LLVOVolume::computeLODDetail`, llvovolume.cpp:1608) and the user's Mesh-detail preference
  (`sLODFactor`) are honoured unmodified; only the distance term is stretched, which is exactly
  what "8x" means: the same angular-size decision made as if the camera were 8 times closer, so
  a 0–256 m stock range becomes 0–2048 m (R3).
- The chosen LOD is requested through `loadMeshStandalone` if not already fetched; what
  *renders* is the best loaded LOD at or below the chosen one — stock's own no-pop rule
  (llmeshrepository.cpp:4527, "don't transition down to avoid funny popping"). LOD requests
  ride the repo's existing score/pending machinery untouched.
- No near-boost ramp, no FOV-zoom factor, no `mLODScaleBias`: a landscape mountain is authored
  knowing its silhouette; the stock ramps exist for objects the camera flies past at prim scale.

## 7. Culling / draw distance

- **Frustum:** one `LLCamera::sphereInFrustum` per instance per frame against the
  world camera; the same camera instance the post-deferred walk renders with.
- **Distance:** the pass ignores the render draw distance — scenery is the point — and culls at
  `SSAtmoLandscapeRenderDistance` (default 2048 = `MAX_FAR_CLIP`, range 256–4096), per-record
  overridable via `render_distance`. Beyond the projection far plane nothing can render without
  the water-style squash trick; the default stays at the far plane and the 4096 ceiling is
  documented as "slices at the far plane" rather than silently supported.
- **Exemptions:** none needed. The landscape is not in any spatial partition, so nothing can
  hide it — no `mInfiniteFarClip` dance, no partition far-clip interplay, no neighbour-region
  blocked-culling question. The renderer is its own cull authority, which is the point of the
  design.

## 8. Failure modes

- **Mesh 404 / purged asset.** The header arrives flagged `m404` and the LOD request resolves
  through the thread's unavailable queue (llmeshrepository.cpp:3517) → `onMeshFailed`. The
  instance renders nothing; the editor row shows an "unavailable" chip. A failed mesh is retried
  only when the environment is re-loaded or the track is crossed — no per-frame retry hammer.
- **Decode failure** (`MESH_PARSE_FAILURE`, empty face list) → same unavailable path; the
  record survives in the schema so a newer/fixed decode can pick it up on next load.
- **No caps / region not loaded:** `constructUrl` warns once
  (llmeshrepository.cpp:1501-1506) and the request sits pending; the renderer renders nothing for
  that record and the editor shows "waiting". A region change re-issues (the adoption diff
  compares against a `mRegionID` stamp and re-requests when it changes).
- **Empty region / no agent region:** step 1–3 no-op; nothing renders, nothing crashes.
- **Track with landscape while the resolver is inactive** (Atmo off, asset unloaded): the
  renderer is gated by the same `want_active` the applier computes, so an inactive resolver
  means no landscape — the schema stays intact in the asset for when it comes back.
- **Same UUID twice in one list:** two instances, one shared `SSAtmoLandscapeMesh` — the
  refcount exists for exactly this.
- **Hand-edited notecard garbage** (bad quaternion, out-of-range x): `fromLLSD` clamps X/Y into
  0–256 and identity-falls a non-normalised rotation; a missing `mesh_id` drops the record with
  a log line. Never trust the hand.

## 9. Performance

- **Idle (no landscape records anywhere):** one bool check per frame in `llviewerdisplay.cpp`
  and one in the pipeline block. Zero.
- **Per frame with scenery:** track-index read, O(records) cache-and-diff, one frustum test per
  instance, GL draws for visible instances only. No octree walk, no `LLDrawable` updates, no
  texture_anim priors — the whole per-frame main-thread cost is a small vector walk plus draws.
- **Draw call budget:** at build time, faces are merged per material slot into one
  `LLVertexBuffer` per slot (concatenated vertices/indices, the model-preview buffer pattern) —
  a 30-face mesh with 3 textures is 3 draw calls, not 30. Budget: ≤ 8 slots per object, ≤ 256
  draw calls per frame scene-wide; overflow culls by angular size (smallest first) — the same
  "small ones on screen" rule the volumetric cloud field already uses.
- **Memory:** decoded `LLVolume`s live only while a live instance uses the mesh (refcounted
  `LLPointer`s); GPU buffers are freed with the mesh entry when its last instance dies. A
  typical backdrop mountain at high LOD is single-digit MB of vertices; 32 records ≈ low
  hundreds of MB worst case, which the per-track cap and the one-active-track-at-a-time rule
  (only the primary track's list ever renders) keep off the common path.
- **Worst case:** 8 tracks × 32 records = 256 records in the asset; at most 32 live instances
  and 8 mesh caches resident (the active track's set, plus adoption overlap during a crossing —
  bounded at 2× the per-track cap for one frame).

## 10. Opt-in / off behaviour

- **`SSAtmoEnabled` off:** the renderer's gate mirrors the applier's exactly — no fetches, no
  draws, live instances killed, mesh cache flushed. No stock path changes behaviour; the four
  stock injections are all behind either the existing Atmo block in `llviewerdisplay.cpp` or a
  `isActive()` check in the pipeline block and the shader is simply never bound.
- **Environment unload:** gate false → same teardown. Stock world returns with zero residue;
  there was never anything in the octree to clean up.
- **Leaving a configured parcel** (parcel discovery swaps assets): the adoption diff runs
  against the new asset; same-UUID meshes ride across, others cut. Instant, consistent with the
  track-crossing idiom.
- **Editor open:** no special case — the renderer reads the working asset, which is what the
  editor mutates; scrubbing, keyframing and reverting behave exactly as every other tab.

## 11. Stock file diff list

Every injection is wrapped in `<SS:Nexii>` … `</SS:Nexii>` tags.

1. **`llviewerdisplay.cpp`** — inside the existing Atmo block (~line 1000), one call:
   `SSAtmoLandscapeRenderer::getInstance()->update();` immediately after
   `SSWaterWorld::getInstance()->update()`, so the renderer reads the same frame's resolved
   track.
2. **`pipeline.cpp`** — in `renderGeomPostDeferred`, beside the existing weather block
   (~4461): a tagged block that, when `cur_type >= LLDrawPool::POOL_SIMPLE` and the WL sky
   group has just finished (a `done_landscape` local, mirroring `done_atmospherics`), calls
   `SSAtmoLandscapeRenderer::getInstance()->render()`. This slots the pass after the sky dome,
   before water — the depth-ordering argument in §2. Gated on the renderer's `isActive()`.
3. **`llviewershadermgr.h` / `llviewershadermgr.cpp`** — `extern LLGLSLShader gSSLandscapeProgram`
   declaration (beside `gSSCelestialProgram`, llviewershadermgr.h:282), `mShaderList` push,
   unload call, and the load block (files `deferred/ssLandscapeV.glsl` / `ssLandscapeF.glsl`,
   `mShaderLevel = mShaderLevel[SHADER_DEFERRED]`, common permutations) — a structural copy of
   the celestial registration at llviewershadermgr.cpp:2089-2101.
4. **`llmeshrepository.h` / `llmeshrepository.cpp`** — the standalone-load extension of §1:
   `MESH_REQUEST_LOD_STANDALONE` in `EMeshRequestType`; `LLMeshRepository::loadMeshStandalone` /
   `cancelMeshStandalone` and the `mStandaloneLoads` map; the dispatch case in
   `notifyLoadedMeshes`; the listener-delivery branches at the top of `notifyMeshLoaded` /
   `notifyMeshUnavailable`; `LLMeshRepoThread::loadMeshLODStandalone`. No existing code path is
   re-ordered; the volume-coupled branches run unchanged after the standalone branch.

New files (all `ss`-prefixed, viewerlgpl header, `LLCachedControl` statics for
`SSAtmoLandscapeRenderDistance`, `SSAtmoLandscapeEnabled` sub-gate if ever needed):
`ssatmolandscape.h/.cpp`, `sspanellandscape.h/.cpp`, `ssLandscapeV.glsl`, `ssLandscapeF.glsl`,
plus the SS-file edits to `ssatmoenvasset.h/.cpp` (record + track list), `ssatmoenvapplier.h/.cpp`
(publish `lastPrimaryTrack()`), `ssfloateratmoenv.h/.cpp` and `floater_ss_atmo_env.xml` (tab).

## What is lost versus the stock path, and what it costs

- **Shadow casting.** Landscape is not in the shadow pass's octree, so mountains cast no sun
  shadows. Phase 2 can inject a depth-only buffer draw into `generateSunShadow`; phase 1 ships
  without it. Receiving shadows is likewise absent in phase 1 (the forward shader reads
  sun/ambient, not the shadow map); the bind-and-sample machinery exists and is the first
  phase-2 item.
- **Reflection probes.** The deferred shade's probe blending never sees landscape fragments
  drawn after it. PBR-on-landscape therefore waits for phase 2 (the schema's `material` field is
  the reservation); phase 1 shades with diffuse + specular-from-sky, which at 300–2000 m reads
  as "distant scenery" rather than "wrong".
- **Alpha sorting.** Blended faces sort per-object, not per-face like `POOL_ALPHA`. Foliage-style
  soft alpha is phase 2; phase 1 renders cutout (`ss_alpha_mode = 1`) which covers fences, rock
  cutouts and most scenery alpha honestly, and per-object back-to-front ordering keeps the
  blended case acceptable.
- **SSAO / deferred fog.** No SSAO on landscape (screen-space pass ran on the gbuffer before
  this pass); mitigated by the slot choice — `doAtmospherics` runs *after* this pass reads the
  depth the landscape wrote, so distant mountains receive the same screen-space haze as world
  geometry, which is the effect that matters at these distances.

The compensating gains are real: mountains haze correctly, occlude and are occluded by world
geometry via the depth buffer alone, appear in water refraction, and occlude the weather passes
behind them — all with zero octree presence and zero server traffic.