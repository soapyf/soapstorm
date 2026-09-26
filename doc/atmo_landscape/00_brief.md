# Atmo Magic Landscape — shared design brief

This document is the single source of truth for the five implementation designs and
their adversarial reviews in this directory. Every design must satisfy every
requirement below. Every claim made against the codebase in a design or review must
be verified by reading the actual code.

## What is being designed

**Atmo Magic Landscape**: mesh scenery (mountains, distant city skylines, floating
islands) that is driven by the Atmo Magic environment asset rather than by rezzed
region objects. The author drags a mesh item from inventory into the landscape
editor; the viewer downloads the mesh asset and renders it client-side; all state
lives in the Atmo Magic environment notecard, so an environment copies across an
estate and every region renders the same scenery.

There is no server object at runtime. The landscape is atmosphere, not content.

## Requirements (all mandatory, from the author's spec)

- **R1 — Drop + fullperm gate.** Drag a mesh inventory item into the landscape
  editor. Only full-perm items are accepted (`LLViewerInventoryItem::getIsFullPerm`,
  llviewerinventory.cpp:2482 — "modify-ok & copy-ok & transfer-ok"). Reject
  non-fullperm drops with a message.
- **R2 — Metadata carry-over.** Name, description, creator, last owner (and any
  other useful inventory metadata) are captured from the dropped item into the
  record. Creator/last-owner are read-only record fields.
- **R3 — Relaxed LOD.** LOD switching distances stretched ~8x relative to stock
  (0–2048 m effective range vs stock 0–256 m), while still honouring the user's
  Mesh-detail preference. Implementation anchor: `LLVOVolume::calcLOD`
  (llvovolume.cpp:1664), `updateLOD` is virtual (llvovolume.h:247), `calcLOD`
  currently is not.
- **R4 — Track lifecycle with UUID adoption.** Landscape objects belong to a track
  (`SSAtmoEnvTrack`). On track crossing, match live objects against the next
  track's landscape list **by asset UUID**: same UUID → keep the loaded object
  instance and adopt the new record's transform/TE settings (no re-download, no
  rebuild pop); UUID only in new track → create; UUID only in old track → kill.
  Transition presentation is an instant cut, consistent with the existing
  "a preset can't be interpolated" idiom for track-boundary swaps.
- **R5 — Multiple mesh objects.** A landscape slot is a list, not a single slot.
  The schema is a per-track list of landscape records.
- **R6 — Region-locked positioning.** Each record has two position modes:
  - *region-locked*: store a region-local offset (X/Y 0–256, Z free — Z is a
    shared sea-level datum); on agent region change, re-anchor into the new region
    (`LLViewerObject::setRegion`, llviewerobject.cpp:7417) so estate copies render
    the mesh at the same spot relative to each region's origin. Editor shows
    region-local coords → what you author is what every region gets.
  - *free*: stored global position; object stays put as regions cross.
  Exactly one instance exists at a time (re-anchored, not per-region copies —
  unlike the SSWater family, which deliberately builds per-region planes).
- **R7 — Persistence in the environment asset.** All state serialises into the
  `SSAtmoEnvAsset` LLSD notecard (pretty XML, hand-editable) via the existing
  manager round-trip, so the existing Bridge/parcel-discovery machinery copies it
  across an estate unchanged.

Additional standing rules from AGENTS.md and the established conversation:

- No builds/compiles by the viewer work itself — designs only; the author builds
  and tests.
- New identifiers use the `SS` prefix, never `FS`. Feature files use the `ss`
  filename prefix in indra/newview.
- Injections into stock files are wrapped in `<SS:Nexii>` … `</SS:Nexii>` tags.
  Every design must list exactly which stock files it touches and how.
- Code style matches fsrezqueue.cpp (LLCachedControl statics for settings,
  viewerlgpl license header).
- Atmo Magic is opt-in end to end (`SSAtmoEnabled`); with the master switch off,
  nothing may run and no stock path may change behaviour.
- Determinism: no `rand()`-seeded noise (LLPerlinNoise is NOT reproducible across
  clients).
- Face textures / PBR materials dropped onto landscape faces should pass the same
  fullperm gate (consistency: the record travels with the estate).

## Verified codebase facts (starting map; verify anything else yourself)

- `gObjectList.adoptViewerObject(LLViewerObject*)` — llviewerobjectlist.cpp:2246,
  header note llviewerobjectlist.h:71-73. Added for the SSWater family: "the caller
  news the object and this does the same bookkeeping createObjectViewer would
  have."
- `SSWater : public LLVOWater` — sswater.h:44-51; constructed directly and adopted
  at sswater.cpp:313; family lifecycle in `SSWaterWorld` (rebuild/kill per region
  change, clearWaterObjects at llworld.cpp:138, per-frame update driven from
  llviewerdisplay.cpp:1000). Rationale in doc/atmo_magic_water.md.
- `LLStaticViewerObject` — llviewerobject.h:1186; base for client-side-only world
  objects (sky, water, WL sky, surface patches).
- Client-side objects enter `gObjectList` via `createObjectViewer` /
  `adoptViewerObject` (llsky.cpp:204 uses `createObjectViewer(LL_VO_SKY, NULL)`).
- Mesh assets: `AT_MESH`, fetched via region `GetMesh`/`GetMesh2`/`ViewerAsset`
  caps keyed by asset UUID (llmeshrepository.cpp:4796-4799, constructUrl at 1458).
  Decode produces `LLVolume` delivered via `LLMeshRepository::notifyMeshLoaded`
  (llmeshrepository.cpp:4963) to waiting `LLVOVolume`s (4996-5002). Main-thread
  request entry is `loadMesh(LLVOVolume*, params, lod)` — volume-coupled delivery.
- Standalone mesh rendering precedent: the mesh upload preview builds its own
  `LLVertexBuffer`s from models and draws them with its own shader, no region
  object (llmodelpreview.cpp:3911-3922, ~5030).
- Atmo Magic already renders non-region geometry: celestial billboards drawn as
  camera-facing quads in the sky pass (lldrawpoolwlsky.cpp:766-824), fed by the
  applier's billboard list.
- Reconcile-loop persistence precedent: the applier's `put()` cache-and-diff
  pattern (ssatmoenvapplier.cpp:982-1013).
- Selection/edits send server traffic that must be gated for any selectable local
  object: `LLSelectMgr::sendSelect` → "ObjectSelect" (llselectmgr.cpp:5524),
  `sendMultipleUpdate` (4892), plus properties and derez paths.
- Env asset schema: ssatmoenvasset.h — tracks (SS_ATMOENV_MAX_TRACKS = 8), per
  track water/weather/planetary/atmosphere/clouds; LLSD round-trip in
  ssatmoenvasset.cpp; manager in ssatmoenvmanager.*; applier/bridge/resolver
  architecture documented in doc/atmo_magic_environment.md.
- Track resolution: `SSAtmoEnvTrackResolver` (trackContaining, crossing events,
  instant-cut vs neighbour blend) in ssatmoenvtrackstate.cpp /
  ssatmoenvbridge.cpp.
- LOD switching: `LLVOVolume::calcLOD` (llvovolume.cpp:1664) — distance from
  `mDrawable->mDistanceWRTCamera`, radius from `mLODScaleBias` × scale,
  `sDistanceFactor`, near-boost ramp, FOV-zoom factor. `DebugObjectLODs` overlay
  works per-object.
- Fullperm: `LLViewerInventoryItem::getIsFullPerm()` llviewerinventory.cpp:2482.
- `LLViewerObject::setRegion(LLViewerRegion*)` llviewerobject.cpp:7417.
- DAD cargo type for mesh items: `DAD_MESH` (llviewerassettype.cpp:81 maps
  AT_MESH → DAD_MESH).
- Existing asset-list drag/drop pattern in Atmo Magic UI:
  ssfloatertexturelist.cpp:175 (`handleDragAndDrop`), ssassetlist.cpp.

## Rules for design documents

Each design doc lives at `doc/atmo_landscape/design_N_<slug>.md` and must cover:

1. **Architecture** — the runtime representation of a landscape object, classes
   (SS-prefixed), files (new `ss*` files; stock files touched with `<SS:Nexii>`
   tags), and where each piece lives.
2. **Data flow** — drop → record → hydration → render; track crossing; region
   crossing; load/unload of an environment.
3. **Schema** — the exact LLSD shape added to `SSAtmoEnvAsset` (per-track
   landscape list, record fields).
4. **Editing UX** — how the author names, positions, textures the landscape
   (stock build tools, dedicated floater, or hybrid), including how undo and
   name/desc properties behave.
5. **Persistence** — when and how state is written to the env asset (event hooks
   vs reconcile loop), and how load restores it.
6. **LOD strategy** — how R3 is implemented without disturbing stock objects.
7. **Culling/draw distance** — behaviour for large/far scenery and any exemptions.
8. **Failure modes** — missing/purged mesh asset, 404, empty region, no caps,
   track with landscape but track resolver inactive, etc.
9. **Performance** — per-frame cost when idle, memory cost, worst case (many
   objects, many tracks).
10. **Opt-in/off behaviour** — what happens on `SSAtmoEnabled` off, on
    environment unload, on leaving a configured parcel.
11. **Stock file diff list** — every stock file touched, what the injection is,
    all tagged `<SS:Nexii>`.

Be concrete: name every class, method, and file. Write prose in the established
doc style (see doc/atmo_magic_water.md and doc/atmo_magic_environment.md — dense,
opinionated, rationale-forward). No code must be written; pseudocode and
signatures are fine.

## Rules for reviews

Each review lives at `doc/atmo_landscape/review_N_<slug>.md` and must:

- Verify every codebase claim in the design against the actual code (grep/read).
  Mark each as CONFIRMED / WRONG / IMPRECISE with the correct fact.
- Attack: technical feasibility, performance, memory, edge cases (teleport,
  region cross, estate copy, editor interactions, undo, other Atmo Magic
  systems), maintainability, stock-diff size, convention compliance (AGENTS.md,
  SS prefixes, tags, opt-in), and anything silently undefined in the design.
- Also extract what is genuinely good and steal-worthy (the synthesis needs it).
- Be adversarial: assume the design is wrong until proven otherwise. No praise
  without verification.
