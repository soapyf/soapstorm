# Atmo Magic Landscape — Design 3: author real, render local

The authoring experience and the runtime have opposite needs, so this design gives each of them
the mechanism the other would ruin. **Authoring** runs on a genuine rezzed region object: the
viewer rezzes the dropped mesh item through the normal `RezObject` path and the author edits it
with the completely stock build floater — faces, PBR materials, transform, undo, everything —
with zero viewer hacks anywhere in the editing path. A capture step then walks the object's true
state and serializes it into the environment record, and the temp object is deleted. **Runtime**
is the mirror image: no server object at all. Each record hydrates into a client-only
`SSLandscapeObject : public LLVOVolume` that reuses the stock volume pipeline wholesale — mesh
fetch, faces, deferred shading, PBR materials, reflections, octree culling — so "reproducing the
authored look" costs no rendering code at all; the record is simply applied to a viewer object
the server never learns about.

The design's identity is the **capture round-trip**: everything that defines the look — mesh
asset, per-face TE state, PBR material IDs, transform, light — serializes to LLSD and hydrates
back with nothing lost, because at capture time the state is literally read off a real object
with stock accessors. That fidelity is also the design's honest boundary: the record is a
snapshot taken at capture, and this document says exactly when snapshots are taken, what happens
when the author keeps editing afterwards, and what an uncaptured object leaves behind.

## 1. Architecture

**The runtime representation is an `LLVOVolume` subclass, not a bespoke vertex-buffer pass.** The
mesh upload preview precedent (own `LLVertexBuffer`s drawn with a private shader,
llmodelpreview.cpp:3911-3922) is the lightest option, and it is the wrong one here: it renders a
fixed simple shader with baked UVs — no deferred pipeline, no PBR probe lighting, no alpha pass,
no reflections. Reproducing the authored look outside the stock pipeline means reimplementing the
deferred material stack, which is exactly the drift this feature cannot afford: the record's
promise is "renders as authored", and the only renderer that keeps that promise without
perpetual catch-up work is the one the author previewed with. So the runtime object is a real
volume in the real pipeline:

| Piece | Home | Role |
|---|---|---|
| `SSAtmoEnvLandscapeRecord` | `ssatmoenvasset.h/.cpp` | the schema struct, a per-track list member; `asLLSD()`/`fromLLSD()` in the established style of every other env struct |
| `SSLandscapeObject : public LLVOVolume` | `sslandscapeobject.h/.cpp` | the client-only runtime object; one per record instance |
| `LLLandscapePartition` | declared in `sslandscapeobject.h` | `LLVolumePartition` subclass with `mInfiniteFarClip = true` — the far-clip exemption |
| `SSLandscapeWorld` (singleton) | `sslandscapeworld.h/.cpp` | lifecycle: hydrate/diff/re-anchor/kill; ticks from `llviewerdisplay.cpp` right after `SSWaterWorld::update()` |
| `SSLandscapeAuthoring` (singleton) + `SSLandAuthoringSession` | `sslandscapeeditor.h/.cpp` | the authoring lifecycle: rez, watch, capture, derez |
| `SSPanelAtmoEnvLand` + `panel_ss_atmo_env_land.xml` | new files | the Land tab: record list, drop zone, capture controls |
| `SSAtmoEnvApplier::activeTrackIndex()` | `ssatmoenvapplier.h/.cpp` | publishes the resolver's per-frame result (new accessor on an SS file) |

`SSLandscapeObject` follows the SSWater family's construction pattern exactly: pcode
`LL_VO_VOLUME` retained so every pcode-keyed check treats it as a plain volume, the pcode factory
cannot build the subclass, so `SSLandscapeWorld` news it directly and hands it to
`gObjectList.adoptViewerObject` (llviewerobjectlist.cpp:2246) followed by
`gPipeline.createObject` — the same two calls `SSWaterWorld::rebuild` makes (sswater.cpp:314-324).
Region is the agent's region, id is a fresh `LLUUID::generate()`, localID stays 0.

The discriminator is `dynamic_cast<SSLandscapeObject*>` behind one static,
`SSLandscapeWorld::isLandscapeObject(LLViewerObject*)` — no flag member is added to
`LLVOVolume`, because unlike water there is no stock counterpart to suppress and therefore no
per-face draw gating anywhere: the runtime objects are just extra volumes the world's octree
renders. The suppression that water needs (`SSWaterWorld::drawsThisFrame`) has no analogue here;
there is nothing to swap with.

Unpickability is inherited, not added: the constructor sets `mbCanSelect = false`
(llviewerobject.h:818), the same one-line trick `LLVOWater` (llvowater.cpp:59), `LLVOSky`
(llvosky.cpp:437) and particle groups (llvopartgroup.cpp:99) use. `LLVOVolume::lineSegmentIntersect`
returns false for such objects at its first check (llvovolume.cpp:4966), so nothing downstream —
hover highlight, the pie menu, rect select, the build tools, `LLToolDragAndDrop`'s pick — ever
sees a landscape object, and the send-gating the brief worries about (`LLSelectMgr::sendSelect`
→ "ObjectSelect", llselectmgr.cpp:5524; `sendMultipleUpdate`, 4892; `sendTEUpdate`,
llviewerobject.cpp:5528) is unreachable by construction: those messages are only ever sent from
selection-driven edit paths. There is no selection-manager diff in this design at all.

The authoring object needs no gating at all — it is a genuine server object, and everything about
it is stock. That is the point.

## 2. Data flow

**Drop → record draft.** The Land tab's drop zone (`SSPanelAtmoEnvLand::handleDragAndDrop`, the
`ssfloatertexturelist.cpp:175` pattern) accepts `DAD_MESH` from `SOURCE_AGENT`/`SOURCE_LIBRARY`
only. The gate is `LLViewerInventoryItem::getIsFullPerm()` (llviewerinventory.cpp:2482);
non-fullperm drops get `ACCEPT_NO` plus a tooltip/message — the item cannot be rezzed into a
record that will be copied across an estate. On acceptance,
`SSLandscapeAuthoring::beginSession(item, record_slot)` snapshots the item's permissions
(`getPermissions().getCreator()` / `getLastOwner()` — R2's read-only fields, taken from the item
because object properties only ever carry them secondhand), checks rez rights (below), and sends
the rez.

**The rez.** `SSLandAuthoringSession::sendRez()` composes the legacy `RezObject` message exactly
as `LLToolDragAndDrop::dropObject` does (lltooldraganddrop.cpp:1971-2028): AgentData with
`FSCommon::getGroupForRezzing()` (fscommon.h:97), RezData with an explicit region-frame ray
(camera origin → a point ~5 m in front of the camera), `RezSelected = true`,
`pack_permissions_slam` on the item's flags and permissions — that packing is file-local in
lltooldraganddrop.cpp, so ~25 lines are duplicated in sslandscapeeditor.cpp rather than
untracked from a stock file. Duplicating is deliberate: it keeps `lltooldraganddrop.cpp` at zero
diffs for this feature. Rez rights are checked client-side first with
`LLViewerParcelMgr::allowAgentBuild()` (llviewerparcelmgr.h:180) — the same predicate that
enables the build tools (lltoolmgr.cpp:338) — because the server's rejection of a rez is silent:
no object ever arrives, and a design that waits for an error will wait forever. The message is
`RezObject`, not a capability; this is the same wire path every inventory rez uses.

On success the object arrives selected (RezSelected), the session grabs it from
`LLSelectMgr::getSelection()`, then sends one `ObjectFlagUpdate`
(`LLViewerObject::updateFlags`, llviewerobject.cpp:7474) setting `FLAGS_PHANTOM` and
`FLAGS_TEMPORARY_ON_REZ` (object_flags.h:41/67) — phantom so it never blocks the author,
temporary-on-rez so the sim will never persist it across a restart and it can never be Taken.
Everything else is stock: the author edits it with the build floater, face textures, PBR
materials, name and description, stock undo, all of it real server state.

**Capture.** `SSLandAuthoringSession::capture()` serializes the live object into a
`SSAtmoEnvLandscapeRecord`:

- mesh asset UUID from `getVolume()->getParams().getSculptID()` — the object's VolumeParams are
  authoritative, not the item (they can only differ if something repathed the object, in which
  case the object wins);
- the full `LLVolumeParams` (sculpt type; the path/profile curve values via the same accessors
  `llvolumemessage.cpp:463-480` uses);
- transform: region-local position, global position, rotation, scale;
- every TE (`getNumTEs()`, `getTE(i)`): texture id, colour+alpha, repeat S/T, offset S/T,
  rotation, glow, the raw `getBumpShinyFullbright()` byte (fullbright rides in it losslessly),
  media flags byte, and the PBR material id from `getTE(i)->getMaterialID()` — mirrored from
  `getRenderMaterialID(te)` (llviewerobject.h:206);
- light parameters when `getIsLight()`;
- name/description from the select node (`LLSelectNode::mName` / `mDescription` — a rezzed object
  has no name of its own until properties arrive; the session object has been selected since rez,
  so they have; item name is the fallback);
- creator/last-owner from the dropped item's `LLPermissions`, captured at drop, never refreshed
  — the object's own creator *is* the author, which is not the record's provenance.

The capture is pure serialization of stock accessors — `LLTextureEntry::getScaleS/getOffsetS/
getRotation/getGlow/getBumpShinyFullbright/getMediaTexGen` (lltextureentry.h:142-163) and
`getMaterialID().asUUID()` (llmaterialid.h:163) — and is therefore lossless by construction: every
value is stored raw, no repacking through quantised forms. Face-texture and PBR references are
checked at capture: each UUID is looked up in the author's inventory and `getIsFullPerm()`-tested
where an item is findable (the standing fullperm rule; see Failure modes for what happens when a
reference cannot be verified).

**Derez.** Capture is followed by derez: select the temp object and send `DeRezObject` to trash
(the stock delete path, llselectmgr.cpp:4431). The object is temporary-on-rez, so a failed derez
is never a leak: the sim will not persist it across a restart, and the session retries each frame
until the object dies. There is no "keep the rezzed copy" option — the record is the deliverable,
a leftover object is litter with land impact, and the whole point of the split is that runtime
regions carry zero objects.

**Hydration → render.** `SSLandscapeWorld::update()` (ticked after `SSWaterWorld::update()`,
llviewerdisplay.cpp:1000, so it reads the same frame's resolved track) runs a rebuild signature —
applier active, `SSAtmoEnabled`, `SSLandscapeEnabled`, active track index, agent region handle,
asset revision counter. A change hydrates the active track's records: for each,
`SSLandscapeWorld::hydrate(record)` news an `SSLandscapeObject`, adopts it into `gObjectList`
(the SSWater pattern, llviewerobjectlist.cpp:2246), registers it with `gPipeline.createObject`,
and calls `setVolume(record's volume params, 0)` — the stock mesh fetch engages by itself
(`LLVOVolume::setVolume` → `gMeshRepo.loadMesh`, llvovolume.cpp:1320). When the mesh arrives the
stock `notifyMeshLoaded()` (llvovolume.cpp:1453) fires; the override applies the record's TEs,
material ids and transform once the face count is real. Render is thereafter ordinary volume
rendering — no custom pass, no shader, nothing registered.

**Track crossing (R4).** The applier's resolved track index changes. `SSLandscapeWorld` diffs the
live set against the new track's records **by mesh asset UUID**, greedily in record order, each
record taking the nearest live object with a matching UUID (nearest by squared position distance
so reuse is visually the cheapest one; list order breaks ties, deterministically — the list order
is hand-editable and stable). A reused object runs `adoptRecord`: transform, TEs, materials,
light — but **not** `setVolume`, since matching by UUID guarantees the asset is the same and the
mesh is already loaded; there is no re-download and no rebuild pop, exactly R4's requirement.
Unmatched new records create; unmatched live objects kill. Presentation is an instant cut — the
same idiom as every track-boundary swap.

**Region crossing.** On agent region change, region-locked records re-anchor: the object calls
`setRegion(new region)` (llviewerobject.cpp:7417 — and its created-list bookkeeping is a no-op
for these objects, `addToCreatedList` guards `local_id > 0`, llviewerregion.cpp:3061) followed by
`setPositionRegion(offset)`. Exactly one instance moves — no per-region copies, the deliberate
opposite of the SSWater family's per-region planes. Free-mode records do nothing. Teleports take
the same path (the resolver's teleported reset lands as an instant cut).

**Load/unload.** An environment load or unload changes the manager's asset; the next
`SSLandscapeWorld::update()` sees the signature move and rebuilds or clears. Unload of an active
environment kills every object (`gObjectList.killObject`, llviewerobjectlist.cpp:1598 — markDead
only, no server traffic) and resets the signature, the SSWaterWorld pattern.

## 3. Schema

Added to `SSAtmoEnvTrack` as an optional key, per R5/R7. No version bump: readers tolerate absent
keys with defaults, in the house `fromLLSD` style. `SS_ATMOENV_MAX_LANDSCAPE = 64` per track,
mirroring `SS_ATMOENV_MAX_TRACKS`.

```
track map gains:

"landscape": [                      # per-track list of records (R5); absent = empty
  {
    "name":        "Distant Ridge",          # object name at capture
    "desc":        "",                       # object description
    "creator":     "uuid",                   # R2, read-only, from the dropped item
    "last_owner":  "uuid",                   # R2, read-only
    "mesh_id":     "uuid",                   # the mesh asset, from VolumeParams' sculpt id
    "mode":        "region",                 # "region" | "free" (R6)
    "offset":      [R.x, R.y, R.z],          # region-local metres; authoritative in region mode
    "global":      [x, y, z],                # world position; authoritative in free mode
    "rot":         [x, y, z, w],             # quaternion
    "scale":       [x, y, z],
    "volume": {
        "sculpt_type": 5,                  # the mesh type byte from LLVolumeParams
        "profile":   [curve, begin, end, hollow],
        "path":      [curve, begin, end, scale_x, scale_y, shear_x, shear_y,
                      twist, twist_begin, radius_offset, taper_x, taper_y,
                      revolutions, skew]
    },
    "faces": [                            # parallel to the mesh's faces; absent keys = TE defaults
      { "tex": "uuid", "color": [r,g,b], "alpha": a,
        "repeat_s": f, "repeat_t": f, "offset_s": f, "offset_t": f,
        "rot": <radians>, "glow": f,
        "bump_shiny": u8,                  # raw bump|shiny|fullbright byte
        "media": u8,                       # media texgen byte
        "material": "uuid-or-empty" }      # PBR render material asset (LLMaterialID as UUID)
    ],
    "light": {                            # present only when the authored object is a light
      "color": [r,g,b], "intensity": f, "radius": f,
      "falloff": f, "cutoff": f, "tex": "uuid" }
  }
]
```

Notes on the shape. `bump_shiny` is stored as the raw `getBumpShinyFullbright()` byte rather than
split, because the byte is the atom the stock wire format moves and splitting it invites a
round-trip bug for zero readability gain. Rotation is a quaternion: euler hand-editing of a
landscape is a rare event, and a quat round-trips exactly through `LLSDSerialize::toPrettyXML`
reals. Both position forms are always written — the capture has both in hand — and `mode` says
which is authoritative (R6). `creator` and `last_owner` are read-only record fields: the editor
shows them, nothing writes them after capture (R2).

## 4. Editing UX

The environment floater gains the **Land** tab the environment IA already names ("Water, Land,
Sky, Space" — doc/atmo_magic_env_ui.md); it slots between Water and Clouds. The panel is a
record list plus a drop zone reading *"Drag a full-perm mesh here"*, mirroring the existing
asset-list drag/drop pattern (ssfloatertexturelist.cpp:175). One drop = one session.

- **Everything else is stock.** The rezzed object is a real object: the author opens the build
  floater, moves it, scales it, textures faces, drops PBR, sets a light — every tool works,
  including undo, because there is nothing to interpose on. Name and description are edited in
  the build floater's General tab like any object and the capture reads them back from the
  selection node. There are no send gates, no property seeding, no selection-manager hooks: the
  server sees an ordinary build session.
- **The capture step.** *Save to environment* (button) serializes the record mid-session; the
  floater's close path serializes again and writes if the fresh capture differs from the stored
  one (`llsd_equals` on the two documents — capture is cheap). If a live session's object still
  exists at close, the editor asks **Save to environment / Discard**; both answers derez the
  object. An author who never saves loses nothing but the object — the record only ever exists
  once captured, so a discarded session leaves the asset untouched and the object is temp-on-rez,
  so the sim self-cleans it at latest on region restart.
- **Edits after saving.** While the session lives, the real object is the preview (the runtime
  suppresses hydration of the record under an open session — the real object *is* the preview,
  WYSIWYG for free). If the author edits after a mid-session capture, the close-time capture
  overwrites the record; the record follows the object, never the reverse.
- **Re-authoring an existing record** ("Edit..." on a record) rezs a fresh temp object at the
  record's stored position wearing the record's state (rezzed, phantom, temp-on-rez, capture
  writes back over the same record slot; creator/last_owner are *not* re-read — the original
  provenance is immutable per R2).
- **No rez rights** — the drop still succeeds, as a proxy record: the mesh id and item metadata
  are captured directly from the inventory item, the record renders client-only immediately, and
  the panel's own offset/rotation spinners (the only non-stock editing surface, shared with the
  fine-tune use case) position it. Full stock-tool editing resumes wherever the author has rez
  rights.

## 5. Persistence

Capture is **event-driven, not reconciled**: the record is written into
`SSAtmoEnvManager::editable()` at capture time and from then on it is an ordinary part of the
asset — `isModified()` (llsd_equals against the baseline, ssatmoenvmanager.cpp:61) picks it up for
free, and `saveNotecard` → `LLSDSerialize::toPrettyXML(mWorking.asLLSD(), ...)` (ssatmoenvmanager.cpp:739)
writes it into the notecard hand-editable, R7 complete. Nothing in the manager, the bridge or the
parcel-discovery machinery changes by a line: the landscape list is one more optional key inside
each track's document, exactly how `mPrecipitationTypes` joined. Load restores via
`SSAtmoEnvLandscapeRecord::fromLLSD` with per-key defaults; unknown keys inside a face map are
ignored, so an environment authored by a newer build still loads its sky, water and weather and
merely drops unrecognized face keys — the same forward-compat stance `mPrecipitationTypes` takes.

The one cache-and-diff in the runtime is the rebuild signature, the SSWaterWorld pattern; records
are static documents, so there is no per-frame put()-diffing to do. The applier's
`activeTrackIndex()` is set each `apply()` from the `SSAtmoEnvTrackResolver` result it already
computes (ssatmoenvapplier.cpp:176-188); `SSLandscapeWorld` reads that, not its own resolution,
so track resolution exists in exactly one place.

## 6. LOD strategy

`calcLOD` becomes virtual — a one-word change in `llvovolume.h` (`updateLOD` already is virtual,
llvovolume.h:247; `calcLOD` at llvovolume.cpp:1664 currently is not). `SSLandscapeObject`
overrides it:

```
bool SSLandscapeObject::calcLOD()
{
    if (mDrawable.isNull()) return false;
    const F32 saved = mDrawable->mDistanceWRTCamera;
    mDrawable->mDistanceWRTCamera = saved / LLViewerAssetStretch();   // SSLandscapeLODStretch, 8.0
    const bool changed = LLVOVolume::calcLOD();
    mDrawable->mDistanceWRTCamera = saved;
    return changed;
}
```

The base formula then runs untouched — `sDistanceFactor`, the near-boost ramp, the FOV-zoom
factor, `sLODFactor` (the user's Mesh-detail preference) all apply as stock, because only the
*input distance* was stretched: LOD behaves as if the camera were 8x closer, so detail survives
out to ~8×256 m ≈ 2048 m, R3 exactly. Stock objects never enter the override — it lives entirely
in the subclass — and the temporary mutation of `mDistanceWRTCamera` is synchronous inside our own
call, restored before any other reader can run. `DebugObjectLODs` works per-object for free
because the base code draws it.

## 7. Culling / draw distance

A volume is octree-culled against the camera far clip, so 2048 m scenery dies at `RenderFarClip`
in the stock `PARTITION_VOLUME`. The runtime objects therefore live in their own partition:
`LLLandscapePartition : public LLVolumePartition` with `mInfiniteFarClip = true`
(llspatialpartition.h:436 — "if true, frustum culling ignores far clip plane"; the water
partitions are the precedent, the water family renders past draw distance for the same reason).
`SSLandscapeObject::getPartitionType()` returns the new `PARTITION_LANDSCAPE`; routing is already
general — `LLPipeline::getSpatialPartition` dispatches through the virtual
(pipeline.cpp:7626-7637). Frustum and octree culling still apply; only the far-clip plane is
exempt, and the partition inherits all of `LLVolumeGeometryManager` — faces, buffers, shadows and
lights included. Its `mDrawableType` stays `RENDER_TYPE_VOLUME`, so the volume toggle in Graphics
preferences governs the scenery too, which is the right answer for a look feature. Raycasts cross
the partition but hit nothing: `mbCanSelect = false` makes `lineSegmentIntersect` decline
(llvovolume.cpp:4966). Region-locked and free records are equal citizens in the partition; there
is one instance, re-anchored, never per-region copies (R6) — the deliberate contrast with the
SSWater family, which builds per-region planes because water *is* per-region.

## 8. Failure modes

- **Missing/purged mesh asset.** `gMeshRepo.loadMesh` fails or the asset 404s; the stock volume
  machinery degrades the way it does for any rezzed object (placeholder cube), the record stays,
  and retry is the stock machinery's problem. The record is never dropped for a transient fetch
  failure.
- **No caps / offline region services.** Same as above — the volume shows its placeholder; a
  later `updateTextures`/LOD cycle re-requests, stock behaviour.
- **No rez rights at the author's position.** Checked before rez with
  `LLViewerParcelMgr::allowAgentBuild()` (llviewerparcelmgr.h:180). Without rights the editor
  falls back to proxy-record authoring (Editing UX above) with a message explaining the limit —
  being explicit about what is possible: the server will not rez for an agent without build
  rights, and a "temporary no-permission" rez is not a thing the protocol offers; the honest
  options were require-rights or degrade, and degrading keeps the feature usable on forbidding
  land. The server re-checks regardless; if a rez that passed the client check still produces no
  object within 10 s (poll the fresh selection the RezSelected flag gives us), the session
  reports failure and stays in proxy mode.
- **Author leaves the region mid-edit.** On agent region change with a dirty session: attempt
  capture (the object is still in `gObjectList` while its region survives in the interest list),
  then derez best-effort; if the object was already killed out from under us (region left the
  interest list), the last capture stands and the session ends. The abandoned server object is
  temp-on-rez: it cannot be Taken, it survives until deleted or until the sim restarts — that is
  the flag's contract, and it is why the flag is set the moment the object arrives rather than at
  capture time.
- **Capture on region cross / environment save mid-edit.** Capture reads only live client state;
  it works from a neighbouring region as long as the object lives. A capture into a session whose
  record slot has been deleted from underneath it re-inserts at the end of the track's list.
- **Object already deleted** (author pressed Delete on their own object): the session's per-frame
  `tick()` notices the dead object, ends the session, and discards the pending capture — the
  stored record (if any) is untouched; a re-edit re-rezzes from the record.
- **Track with landscape but resolver inactive**: the applier's `want_active` gate (Atmo off, no
  asset, no tracks) makes `SSLandscapeWorld` inactive; records render when their track becomes
  the resolved primary and not before. Landscape data is inert metadata to a viewer with Atmo
  disabled — it round-trips through save/load and renders nothing.
- **Varregions**: region-locked offsets are stored region-local and clamped into the target
  region's `[0, width]` on re-anchor; the brief's 0–256 band is the standard-region case and the
  clamping is the defined behaviour otherwise.
- **Fullperm references capture cannot verify** (face textures/materials not in the author's
  inventory): capture proceeds and the panel reports which UUIDs were unverifiable, in one line.
  The gate R1 demands is hard-enforced at the drop; the capture check is the portability warning,
  because referenced textures render for every client exactly as rezzed content does — UUID fetch
  is not permission-gated client-side.

## 9. Performance

Idle per-frame cost is the `SSLandscapeWorld::update()` signature compare — a handful of scalar
compares plus an integer — with the track diff running only when the track index, region handle
or asset actually changes. The cross-track matching is O(live × records) with a mesh-id
hash map in front of it; at the 64-per-track cap and 8 tracks it is a few thousand hash lookups
on the frames where a crossing happens, nothing per-frame. Runtime objects are ordinary volumes
in their own empty-but-real partition: render cost is exactly a rezzed mesh of the same asset,
fetch costs are the stock mesh cap path with no new machinery, and the per-region partition adds
an empty octree on regions with no landscape. Memory is the record documents plus one
LLVOVolume-equivalent per active-track record — the same footprint a rezzed copy would have,
minus the server object. The authoring session adds one real object to the region: land impact
and prim quota are consumed while the author edits (honest cost of authoring real), and zero at
every other time.

## 10. Opt-in/off behaviour

- **`SSAtmoEnabled` off**: the applier never activates, so `SSLandscapeWorld` sees
  `want_active == false`, kills its objects and idles; authoring is a UI action inside the
  environment editor, which itself is Atmo Magic's surface, so nothing atmo runs at all. The
  stock volume path is untouched — there is nothing to restore because nothing stock was
  suppressed; the landscape partition just sits empty.
- **`SSLandscapeEnabled` off** (the per-feature gate): same as above with the master switch on —
  records load and save as data, nothing renders, authoring is refused with a message. This is
  the benchmarking switch.
- **Environment unload** (`SSAtmoEnvManager::unload`): the signature moves, the runtime kills its
  objects; an open authoring session is unaffected (it is a real object session, not a runtime
  artifact) but its "Save" now targets an environment the manager no longer holds — the panel
  refuses with a message, and close derezs the object either way.
- **Leaving a configured parcel**: `SSAtmoEnvDiscoveryManager` tears the environment down through
  the manager; the runtime deactivation is the same kill path. Records are not per-parcel — they
  belong to the track, which is why the runtime keys on the applier's resolved track and not on
  parcel state.
- With the editor closed and no asset loaded, the feature's total runtime presence is one
  signature comparison per frame in a `<SS:Nexii>` block.

## 11. Stock file diff list

Every injection wrapped in `<SS:Nexii>` … `</SS:Nexii>`:

| Stock file | Injection |
|---|---|
| `indra/newview/llviewerdisplay.cpp` | one call, `SSLandscapeWorld::getInstance()->update();`, immediately after `SSWaterWorld::getInstance()->update()` inside the existing `<SS:Nexii>` Atmo block (llviewerdisplay.cpp:1000) |
| `indra/newview/llvovolume.h` | `calcLOD` becomes `virtual` — one word (llvovolume.h, next to `updateLOD`'s override at line 247) |
| `indra/newview/llviewerregion.h` | `PARTITION_LANDSCAPE` added to the partition enum (after `PARTITION_VOLUME`, line 99) |
| `indra/newview/llviewerregion.cpp` | one `mImpl->mObjectPartition.push_back(new LLLandscapePartition(this));` beside the water partitions at line 724-725 |
| `indra/newview/app_settings/settings.xml` | `SSLandscapeEnabled` (bool, default true) and `SSLandscapeLODStretch` (F32, default 8.0) |
| `indra/newview/skins/default/xui/en/floater_ss_atmo_env.xml` | the Land tab button and panel include |

Conspicuously **not** touched, and why: `lltooldraganddrop.cpp` (the rez message is composed in
sslandscapeeditor.cpp — duplication over coupling), `llselectmgr.cpp` (runtime objects are never
pickable via `mbCanSelect`, so `sendSelect`/`sendMultipleUpdate`/properties paths are unreachable
for them; authoring objects are real and need no gating), `lltooldraganddrop.cpp` (the panel's
drop handling is the established floater `handleDragAndDrop` pattern, not a DAD-dictionary
change), `llviewerobjectlist.h` (`adoptViewerObject` already exists), and every shader file (the
runtime renders through the stock volume pools — there is no swap to gate, unlike water).

New files, all `ss`-prefixed, viewerlgpl headers, fsrezqueue style (`LLCachedControl` statics):

- `sslandscapeobject.h/.cpp` — `SSLandscapeObject : public LLVOVolume`, `LLLandscapePartition`,
  `applyRecord()`, `adoptFrom()`, `calcLOD()`/`getPartitionType()`/`notifyMeshLoaded()` overrides
- `sslandscapeworld.h/.cpp` — `SSLandscapeWorld` singleton: `update()`, rebuild signature,
  track-crossing diff, region re-anchor
- `sslandscapeeditor.h/.cpp` — `SSLandscapeAuthoring` + `SSLandAuthoringSession`
- `panel_ss_atmo_env_land.cpp/.h` + `skins/default/xui/en/panel_ss_atmo_env_land.xml`
- `ssatmoenvasset.h/.cpp` — `SSAtmoEnvLandscapeRecord`, the per-track list, LLSD round-trip
- `ssatmoenvapplier.h/.cpp` — `activeTrackIndex()` accessor

## What this buys, and what it costs

**Buys.** Editor fidelity is total: face textures, PBR materials, repeats/offsets/rotation,
alpha modes, light, undo, name/desc — all exercised through the stock build floater on a stock
object, so there is nothing to drift and nothing to benchmark against stock; the runtime
reproduces the captured look through the stock volume pipeline, so the look the author saw while
editing is the look every region renders. The editing-phase core diff is literally zero, and the
runtime's stock surface is four small tagged edits. Capture-on-close with a live WYSIWYG object
makes "what you authored is what the record says" the default rather than a discipline.

**Costs.** Authoring needs rez rights where the author stands (mitigated by proxy authoring, but
proxy authoring is floater-spinners editing, not stock tools — a visibly lesser mode). Authoring
leaves real server objects in a region: prim quota is spent while the session lives, an
abandoned session can leave a temp-on-rez object until sim restart, and the capture step is a
second place the truth lives — an author who edits after saving and then kills the viewer
without closing the floater loses edits since the last capture. Two object lifecycles (the
session's server object and the runtime's client object) must be kept from meeting: the
suppression rule ("a record with a live session renders nothing client-side") is load-bearing
and is the first thing a review should try to break. And the capture is a snapshot, so the
runtime never tracks the author's live object — by design, but the design lives or dies on the
capture hooks being called every time the state leaves the session.
