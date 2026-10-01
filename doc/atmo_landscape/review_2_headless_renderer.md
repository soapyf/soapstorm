# Review: Design 2 (headless renderer)

Adversarial review of `design_2_headless_renderer.md`. Every codebase claim below was
checked against the source in this worktree. The design's core bet — that a headless
renderer with zero object/pipeline presence can satisfy R1–R7 — is mostly sound, and
the mesh-repository extension is genuinely well-grounded in the real machinery. But
the render-slot story contradicts itself, the far-plane problem silently defeats the
headline use case at stock settings, and the request-scoring claim glosses over a real
starvation mechanism.

## Verification of codebase claims

| Claim | Verdict | Correction / note |
|---|---|---|
| Celestial billboards drawn as quads into the sky pass from the applier's list, lldrawpoolwlsky.cpp:766-824 | CONFIRMED | The `gSSCelestialProgram` binds sit at 788-810 (moon) and just above for the sun; `mBillboards` fed by the applier (ssatmoenvapplier.cpp:977-1001). Nuance: this is the **deferred** WL-sky pass (`getNumDeferredPasses`), not post-deferred. |
| SS weather passes draw into the post-deferred pool walk from own singletons, pipeline.cpp:4463-4466 | CONFIRMED | Exactly 4463-4466 (`renderFlash`, `SSVolCloud::render`, lightning, precip), inside the `atmospherics_pass` trigger, gated `!gCubeSnapshot` (4461). |
| Upload preview builds vertex buffers from decoded faces, no region object, llmodelpreview.cpp:3911-3922 | CONFIRMED | Buffer build at 3911-3922; draw at ~5030-5033. "with its own shader" is IMPRECISE: the draw path at 5030 uses `gGL` fixed colour + `LLVertexBuffer`, not a custom program. |
| `loadMesh(LLVOVolume*, params, lod)` registers waiting volume in `mLoadingMeshes`, llmeshrepository.cpp:4482 | CONFIRMED | Signature also carries `last_lod`. |
| `notifyMeshLoaded` (4963) drops the decoded volume when nobody waits | CONFIRMED | The `if (volume && obj_iter != end())` guard (4970) means an unwatched volume is released on scope exit. |
| `mMeshHeader` is `std::unordered_map<LLUUID, LLMeshHeader>` | CONFIRMED | llmeshrepository.h:455-456. |
| `PendingRequestUUID` (h:273) already carries skin/decomposition/physics requests by bare UUID | IMPRECISE | It carries **skin** (cpp:5077). Decomposition/physics ride plain `std::queue<LLUUID>` (`mPendingDecompositionRequests`, h:969) dispatched *outside* the scored `mPendingRequests` path (cpp:4881-4892). |
| `EMeshRequestType` at h:66-74; new type served by `PendingRequestUUID(mesh_id, type)` | CONFIRMED | Adding a type **requires** the `notifyLoadedMeshes` switch case (cpp:4855-4874) — the `default:` is `LL_ERRS`. Design does plan the case. |
| Thread-side `loadMeshLOD(mesh_id, params, lod)` is private, cpp:1362 | CONFIRMED | Private at h:648-663; a member wrapper can call it. Header routing, byte ranges, `constructUrl` (cpp:1464, comment at 1458), cache read and `lodReceived` (cpp:2481) are indeed volume-free. |
| `notifyLoadedMeshes` dispatch switch at 4855-4874 | CONFIRMED | |
| `DAD_MESH` mapping at llviewerassettype.cpp:81 | CONFIRMED | |
| `getIsFullPerm()` llviewerinventory.cpp:2482 = modify & copy & transfer | CONFIRMED | Comment at 2486. |
| `getProtectedAssetUUID()` is null for anything short of full-perm | CONFIRMED | llviewerinventory.cpp:2464-2480 — with one exception: godlike agents get the UUID regardless (2473-2474). The explicit `getIsFullPerm` gate covers it. |
| Applier `want_active` gate at ssatmoenvapplier.cpp:142-155 | CONFIRMED | |
| Resolver call the renderer would publish from, ssatmoenvapplier.cpp:176 | CONFIRMED | `SSAtmoEnvTrackResolver::resolve` (class at ssatmoenvtrackstate.h:38); `blend.mPrimaryTrack` at 183. `lastPrimaryTrack()` does not exist yet — new SS-file API, as stated. |
| `put()` cache-and-diff idiom at ssatmoenvapplier.cpp:545 | CONFIRMED | (The brief's own citation of 982-1013 for `put()` is wrong — that range is the billboard list; 545 is the lambda.) |
| Atmo block in llviewerdisplay.cpp, insertion after `SSWaterWorld::update()` ~1000 | CONFIRMED | Applier at 995, water update at 1000, rain-shadow capture at 1002. |
| `gSSCelestialProgram` decl llviewershadermgr.h:282; load block cpp:2089-2101 | CONFIRMED | Exact range; `mShaderList` push pattern exists (llviewershadermgr.cpp:448+). |
| `calcLOD` llvovolume.cpp:1664, not virtual; `computeLODDetail` 1608; `sLODFactor` | CONFIRMED | h:433 non-virtual; formula `(lod_factor*radius)/distance` at 1614 matches the design's stretched variant. |
| `LLVolumeLODGroup::getDetailFromTan` | CONFIRMED | Declared indra/llmath/llvolumemgr.h:53. |
| No-pop rule "don't transition down", llmeshrepository.cpp:4527 | CONFIRMED | SH-641 comment at 4527. Note: it lives in the volume-coupled `loadMesh`; the headless renderer must reimplement it, which the design says it will. |
| 404 → `m404` header flag → unavailable queue → dispatch at cpp:3517 | CONFIRMED | h:429; pushes at 2344/2389/3650; dispatch 3500-3518. |
| No caps / no region: `constructUrl` warns once (1501-1506), "the request sits pending" | WRONG | `fetchMeshLOD` pushes to `mUnavailableQ` the moment the URL is empty (cpp:2284-2293) → `notifyMeshUnavailable` → `onMeshFailed` **immediately**. There is no pending state; the "waiting" chip and the region-change re-request are the same failed state plus a retry policy. Behaviour converges, the mechanism as described does not exist. |
| `SS_ATMOENV_MAX_TRACKS = 8`; `mWeatherSourceDeck` as non-keyframed property precedent; track `asLLSD` | CONFIRMED | ssatmoenvasset.h:44, 487-492, 496-497. `mLandscape` correctly absent today; ssatmoenvasset is an SS file. |
| Manager API: `hasAsset`/`unload`/`editable`/`isModified`/`saveNotecard`/`mBaseline` | CONFIRMED | ssatmoenvmanager.h:46, 48, 59, 61, 112, 134. |
| `SSTextureListCtrl::handleDragAndDrop` pattern, ssfloatertexturelist.cpp:175 | CONFIRMED | |
| `MAX_FAR_CLIP` = 2048 | CONFIRMED | indra/llmath/llcamera.h:46. |
| Overflow culls by angular size, "the same rule the volumetric cloud field already uses" | WRONG | No such rule exists in ssvolcloud.cpp — the field is a noise-driven cell deck with no per-object angular-size culling anywhere in the file. The policy may be fine; the precedent is invented. |
| Water refraction picks the landscape up | CONFIRMED (conditional) | waterF.glsl samples `screenTex` for refraction (class3/environment/waterF.glsl:87, 295) — true **only if** the landscape draws before POOL_WATER into the same buffer, which the design's stated slot contradicts (see below). |

Also verified: `LLDrawPoolWLSky` overrides only `getNumDeferredPasses() { return 1; }`
(lldrawpoolwlsky.h:48) and inherits `getNumPostDeferredPasses() { return 0; }`
(lldrawpool.cpp:202-206) — it renders in `renderGeomDeferred` (pipeline.cpp:4250). The
pool-type order is POOL_SKY=1, WATEREXCLUSION=2, WL_SKY=3, SIMPLE=4 … WATER=18,
ALPHA_POST_WATER=21 (lldrawpool.h:57-79).

## Fatal / major findings

1. **The render slot as written cannot exist.** §11 places the call "beside the
   existing weather block (~4461)" — but that block lives inside the
   `cur_type >= atmospherics_pass` branch, where `atmospherics_pass = POOL_ALPHA_POST_WATER`
   (pipeline.cpp:4411, 4453). That is **after water, after atmospherics**, not "after the
   sky dome, before water" as §2/§7 claim. Placed there, the design's own payoff list
   breaks: `doAtmospherics` has already run (no haze on mountains — contradicting the
   §"SSAO/deferred fog" argument that "doAtmospherics runs after this pass reads the
   depth the landscape wrote"), water has already drawn (no refraction pickup —
   contradicting the waterF.glsl mechanism), and landscape would paint **over** water
   and weather. The intended slot needs its own loop-top branch — but the design's
   proposed trigger half is also fiction: "the WL sky group has just finished" can
   never be detected in `renderGeomPostDeferred`, because the WL sky pool has zero
   post-deferred passes and renders in the *deferred* walk (verified above). What
   survives is only the bare `cur_type >= POOL_SIMPLE` position — implementable, but
   then the doc's depth-ordering argument, its line citation, and its trigger
   description are all wrong in mutually reinforcing ways. This is the load-bearing
   wall of the design and it is currently built on a misreading of the loop.

2. **Pool-existence fragility: the feature vanishes in empty views.** A loop-position
   trigger fires only if some pool with `type >= POOL_SIMPLE` exists that frame — pools
   exist only when faces exist. The product's headline scene (a sky platform over a
   tracked build, nothing rezzed, no visible water, looking at a 2 km backdrop range)
   can produce *no* such pool, so the landscape never draws. `done_atmospherics` has
   the same structural hole and stock accepted it because the loss is a missing fog
   pass; here the loss is the entire feature in exactly the scenes it exists for. The
   fix is cheap (latch to the always-visited `POOL_WL_SKY` iteration — the pool exists
   whenever the deferred sky pass ran — or render after the loop unconditionally), but
   the design neither states nor solves the problem.

3. **The far plane defeats the default configuration.** The projection far plane is
   `gAgentCamera.mDrawDistance` (llviewercamera.cpp:340); `MAX_FAR_CLIP` (2048) applies
   only to special cameras. The design's cull default of 2048 m is therefore moot: a
   user at stock draw distance has a far plane at 128–512 m and the 2 km backdrop is
   projection-clipped no matter what the pass culls at. §7 concedes "beyond the
   projection far plane nothing can render without the water-style squash trick" — and
   then neither implements nor schedules it, not even as phase 2. The in-house
   precedent it name-checks exists precisely for this (sswater.h:34, ssvolcloud.h:44 —
   "far-field squash cap as a fraction of MAX_FAR_CLIP"). As written, at default
   settings the feature renders nothing in its primary scenario. Squash or an
   explicit far-plane policy must be phase 1.

4. **Standalone requests score 0 and starve.** §6 claims LOD requests "ride the repo's
   existing score/pending machinery untouched". Mechanically true, but
   `PendingRequestBase::updateScore` (llmeshrepository.cpp:608-622) is a max over
   `calculate_score(volume)` of tracked `LLVOVolume`s — a headless request has no
   tracked data, so its score is permanently 0 and it sorts last under
   `CompareScoreGreater` (cpp:4846-4848) whenever pending exceeds the high-water mark.
   During a busy teleport the scenery requests lose to every object LOD, indefinitely
   (there is no expiry). Maybe lowest-priority scenery is the *right* policy — but it
   must be a stated policy with a floor (e.g. score landscape requests by their own
   angular size), not an accident of a missing volume.

5. **Missing gates the weather precedent already has.** The weather block is gated
   `!gCubeSnapshot` (4461) and atmospherics self-skip under
   `LLPipeline::sRenderingHUDs` (4403). §11 gates the landscape block only on
   `isActive()`. Unhandled consequences: the pass runs again inside reflection-probe
   cube snapshots (6 faces per probe, unmanaged cost, unstated decision) and inside the
   HUD post pass (mountains drawn with HUD state). Both need explicit gates; both are
   one line each, but their absence shows the slot was never walked against real
   callers of `renderGeomPostDeferred`.

6. **The editing story is honest but second-class, and the design under-describes it.**
   Placement is numeric spinners in a floater: no in-world gizmos, no click-to-select
   (nothing is pickable — you cannot point at a mountain and know which record it is),
   no camera-relative placement, no keyboard nudging, no visible selection highlight,
   no per-record hide toggle. For a 32-row list of large scenery this is workable for
   coarse placement and painful for composition (aiming a rotation by typing XYZ
   degrees against a backdrop 800 m away is trial-and-error). "Live preview is free"
   is real (the per-frame diff shows edits immediately) and the fullperm gate dissolves
   the ownership misery of build tools — but the doc should say plainly that the author
   gives up the entire stock manipulation toolkit, and budget for at least a
   highlight-in-world overlay and camera-relative nudge keys before this is credible
   for a scenery author.

7. **More than 8 material slots is undefined.** §9 states "≤ 8 slots per object" as a
   budget, but scenery meshes routinely carry more distinct face textures. No rule is
   given for what happens: merge? drop faces? refuse the drop? Same class of gap:
   a **rigged** mesh item passes the DAD/fullperm gate, and the design never says
   whether it is refused at the header (skin-info presence is knowable from the
   header) or silently rendered in bind pose.

## Minor findings

- `mStandaloneLoads` "keyed by UUID" but delivery checks "this mesh/lod" — the key must
  be (uuid, lod) or a uuid→lod-set map. One-line fix, but the doc should say which.
- §8's retry policies conflict internally: bullet 1 says a failed mesh is retried "only
  when the environment is re-loaded or the track is crossed", bullet 3 says a region
  change re-issues it. Reconcile into one retry matrix.
- `sDynamicLOD == false` branch of `computeLODDetail` (llvovolume.cpp:1617-1620) is
  ignored; users who disabled dynamic LOD get the angular rule for landscape anyway.
  Trivial, but it is a silent deviation from "honour the user's LOD preference".
- `LLVolume` has no whole-volume bounding-box accessor; the §6 radius must come from
  per-face bounds (`LLVolumeFace::mBounds`) or a walk over `getMesh()`. Fine, just not
  "from the decoded volume's bounds" as if a getter exists.
- Memory accounting ("low hundreds of MB worst case") excludes face-texture residency
  entirely and says nothing about the texture fetch/eviction path for face slots —
  LLViewerTexture lifecycle, boost priority, missing-texture placeholder are all
  unaddressed. Also: a mesh that is *also* rezzed in-world gets decoded twice
  (standalone volume never enters the system volume manager, by design).
- Blended faces get no atmospherics treatment (doAtmospherics is depth-based; blended
  faces should not write depth) — the §"lost" list only discusses SSAO. Per-object
  back-to-front ordering across *instances* is asserted but no sort key is specified.
- The "godlike exception" to `getProtectedAssetUUID` means the payload call alone does
  not enforce the gate; the design's separate `getIsFullPerm` check is load-bearing —
  keep both.
- World-Z for landscape positions is compliant with the brief's "shared sea-level
  datum" but breaks the SS convention that track-space quantities are floor-relative
  (`SSAtmoEnvTrack::mFloorZ` frames water and both decks). A sky build at floor 2000
  forces the author to type absolute world Z while every other tab dials relative
  numbers.
- `constructUrl` is at cpp:1464 (the doc comment is at 1458); `SSWater` adoption is at
  sswater.cpp:314. Off-by-a-few, harmless.
- Undo is session-local to the panel and dies with the floater; durable safety is
  Save/Revert. Stated, acceptable, but Ctrl+Z habits from build tools will not carry.
- Editor live-preview works because the renderer diffs the working asset — but §5's
  "event hooks, not a reconcile loop" then describes exactly a per-frame reconcile
  loop; the terminology fights the architecture it praises.

## Silently undefined behaviour

- **Advanced lighting off.** The shader lives in the deferred folder and mirrors the
  celestial registration (`mShaderLevel[SHADER_DEFERRED]`); at class 0 it never
  compiles and the landscape silently never renders. Nowhere stated.
- **Cube snapshots / reflection probes:** in or out, at what cost, with what lighting?
- **HUD pass** (`sRenderingHUDs`) — see major finding 5.
- **Rigged meshes** dropped into the target — accept, refuse, or bind-pose?
- **Mesh with > 8 distinct face textures** — see major finding 7.
- **Colour space:** the post-deferred target's gamma/sRGB conventions (the SS
  programs set `hasGamma`/`hasSrgb` features; the celestial fragment shader had to
  match them) are never mentioned; a custom fragment shader that guesses wrong will
  look washed out or dark next to world geometry.
- **Uniform plumbing for sun/ambient** ("diffuse + specular-from-sky"): who feeds the
  shader, from which resolved track state, per frame — unstated.
- **Behaviour when the agent's region has no caps at drop time** (offline preview vs
  fetch failure) — only the render-side failure is covered.
- **What `isActive()` means** for the pipeline gate (mirror of `want_active`? mesh
  count > 0?) — named, never defined.
- **Two instances sharing a mesh but different LOD choices** (different scales): the
  shared `SSAtmoLandscapeMesh` is keyed by UUID but the LOD is per-instance in the
  rule — who owns the per-LOD volumes? (The design's "decoded LLVolume per requested
  LOD" implies the mesh entry holds several; say so.)

## Steal-worthy parts

- **The standalone mesh-fetch extension.** Verified end-to-end against the real
  machinery: UUID-keyed thread state (`mMeshHeader`, h:455), reusable header/byte-range/
  cache/decode path (`loadMeshLOD` private entry at cpp:1362, `lodReceived` at 2481),
  a `PendingRequestUUID`-typed request with a mandatory dispatch case, and a delivery
  branch ahead of the volume-coupled lookup in `notifyMeshLoaded`/`notifyMeshUnavailable`
  (both main-thread, cpp:4963/5010 — the "volume dropped when nobody waits" fact is
  real and the listener hand-off is safe). Any future headless mesh consumer — the
  landscape, a sky-line tool, a preview surface — should take this design nearly
  verbatim, including the synthetic-`LLVolumeParams` trick and "leave the in-flight
  HTTP request alone on cancel".
- **Publish the applier's resolved primary track index** (`lastPrimaryTrack()`) instead
  of re-running the resolver — one source of truth, renderer and sky cannot disagree.
  Cheap, correct, generalises to any future track consumer.
- **Computed, not stored, transforms.** Region-locked records resolve
  `originGlobal + (x, y, 0)` per frame; the whole re-anchor/setRegion/one-instance
  problem class (R6) disappears by construction, and the design correctly contrasts
  itself with the SSWater per-region plane family.
- **The UUID-adoption diff (R4)** with a refcounted mesh cache: same-UUID keeps volume,
  buffers and textures across track crossings and parcel swaps; only transforms/TEs
  re-apply. This is the cleanest lifecycle in any of the five designs.
- **Schema forward-compat discipline:** the `material` phase-2 reservation, both
  position modes always emitted, per-field defaults, missing-key tolerance, and the
  explicit refusal to bump the schema version — all match the established
  `mPrecipitationTypes`-era practice (verified, ssatmoenvasset.h:580).
- **Failure-state UX taxonomy** (waiting / unavailable chips, retry-on-region-change
  stamp) — worth keeping even after the mechanism descriptions are corrected.

## Verdict

**Feasibility: 6/10.** The architecture (no object, no octree, no send-gating) is the
right call and the stock-diff blast radius is genuinely small — four stock files, each
a tagged block, all behind the existing opt-in, SS prefixes and cached-control style
throughout. But the design currently cannot be implemented as written: the render slot
is self-contradictory and half of its trigger mechanism is fictional, the far-plane
problem quietly disables the headline use case at stock settings, and the request
scoring it leans on silently starves headless fetches.

**Three worst problems:**
1. The render slot: "beside the weather block (~4461)" is after water; the "WL sky just
   finished" detector cannot exist in the post-deferred walk — pick a real, defended
   trigger and re-derive the depth-ordering claims from it.
2. Far-plane clipping vs the 2048 m default: steal the SSWater/SSVolCloud far-field
   squash or fail the primary scenario.
3. Score-0 starvation of standalone mesh requests plus the pool-existence fragility of
   any pool-walk trigger — both are "the feature silently doesn't happen" failure modes.

**Three best ideas:**
1. The `MESH_REQUEST_LOD_STANDALONE` repository extension — verified feasible, reusable
   far beyond this feature.
2. Computed region-locked transforms (re-anchoring as arithmetic, one instance ever).
3. The UUID-adoption diff with refcounted mesh cache, driven by the applier's published
   primary track.
