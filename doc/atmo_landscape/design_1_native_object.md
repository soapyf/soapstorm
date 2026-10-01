# Design 1: the native local object — a real LLVOVolume with stock mesh pipeline and stock build tools

Atmo Magic Landscape as a **native local object**: the mountain on screen *is* an
`LLViewerObject` — specifically an `LLVOVolume` subclass constructed directly, bypassing
the pcode factory exactly the way the SSWater family is constructed — carrying the mesh
asset UUID in its `LLVolumeParams`, so the entire stock mesh pipeline (GetMesh fetch,
decode, LOD, faces-per-submesh, per-face TEs, draw pools, octree) works on it without one
line of bespoke geometry code. Editing happens with the stock build tools by making the
selection machinery safe for a server-less object: every server send is gated behind an
`isLocalContent()` flag, selection nodes are seeded from the landscape record, and pie-menu
Delete removes the record. Persistence is a **reconcile loop**, not event hooks: per frame
the live object's state is serialized, diffed against the last snapshot (the applier's
`put()` cache-and-diff pattern, ssatmoenvapplier.cpp:545-553), and diffs write into the
`SSAtmoEnvAsset` record. Every editor path — manip tools, face panel, texture drops,
name/desc, LOD pin — lands as client-side state on the VO or its `LLSelectNode`, so they
all converge through one funnel and there is no commit step to miss or to undo.

The thesis of this strategy: the viewer already has a first-class mesh renderer, a
first-class mesh editor, and a first-class way of holding a loaded mesh (`LLVOVolume` +
`LLVolume` + draw pools + octree). The upload-preview path (`llmodelpreview.cpp:3911-3922`,
~5030) proves meshes can render outside the object system, but it stops at "renders" — no
culling, no pools, no per-face textures, no tools. `adoptViewerObject` was added for the
water family precisely to give caller-constructed objects full object-list citizenship
(llviewerobjectlist.cpp:2246, header note llviewerobjectlist.h:71-73); using it for a
*volume* means the landscape gets everything the mesh pipeline does for a rezzed object,
for the price of one flag and a handful of gates.

## 1. Architecture

### 1.1 Classes and files

| Piece | File | Base | Role |
|---|---|---|---|
| `SSAtmoLandscapeObject` | `ssatmolandscape.h` | `LLVOVolume` | the live landscape object; constructed directly, adopted |
| `SSLandscapePartition` | `ssatmolandscape.h` | `LLVolumePartition` | far-clip-exempt octree for landscape objects (§7) |
| `SSAtmoLandscapeWorld` | `ssatmolandscape.cpp` | `LLSingleton` | lifecycle, adoption, reconcile, gates (the `SSWaterWorld` role) |
| `SSAtmoLandscapeRecord` | `ssatmolandscape.h` | — | the schema struct; one per landscape slot (R5) |
| `SSPanelAtmoEnvLandscape` + `SSLandscapeListCtrl` | `ssatmolandscapeui.*` | `LLPanel` / `LLUICtrl` | the Landscape tab: list management, drop target, LOD pin, mode |
| `panel_ss_atmo_landscape.xml` | skins xui | — | the tab's XML |

`SSAtmoLandscapeObject` reuses the **stock volume pcode** (`LL_PCODE_VOLUME`), exactly as
`SSWater` reuses `LL_VO_WATER`: the pcode factory cannot build the subclass, so the world
news it directly and hands it to `gObjectList.adoptViewerObject` (the SSWater pattern at
sswater.cpp:314). Sharing the volume pcode means every pcode-keyed check in the viewer —
octree debug colours, the static-drawable asserts, pie-menu availability, highlight
silhouettes — treats the landscape object *identically to a stock mesh object*, and
nothing needs registering anywhere. The only discriminator is `isLocalContent()`
(llviewerobject.h, see §11).

Construction follows `SSWaterWorld::rebuild` (sswater.cpp:314) plus the volume-specific
hydration the water planes don't need:

1. `LLUUID id; id.generate();` — a fresh viewer-side id (`mLocalID` stays 0; no server
   object exists, and `LLViewerRegion::addToCreatedList` no-ops on local id 0,
   llviewerregion.cpp:3061-3067, so `setRegion` is safe).
2. `new SSAtmoLandscapeObject(id, LL_PCODE_VOLUME, agent_region)`; constructor sets
   permission flag bits (`FLAGS_OBJECT_MODIFY/COPY/MOVE/OWNER` family) and
   `setLocalContent(true)`.
3. `gObjectList.adoptViewerObject(vo)` — map + list + `updateActive` exactly like
   `createObjectViewer`'s tail.
4. `gPipeline.createObject(vo)` — creates the drawable (SSWater precedent).
5. `setVolume(params, LOD)` with `LLVolumeParams` carrying `setSculptID(mesh_asset_id,
   LL_SCULPT_TYPE_MESH)` (LL_SCULPT_TYPE_MESH = 5, indra/llmath/llvolume.h:194).
   This is the one call that buys the whole pipeline: `LLVOVolume::setVolume`
   (llvovolume.cpp:1229) recognises MESH sculpt type and issues
   `gMeshRepo.loadMesh(this, params, lod, last_lod)` (1320) — region `GetMesh`/
   `GetMesh2`/`ViewerAsset` caps keyed by asset UUID (llmeshrepository.cpp:4482,
   4796-4799) — and decode delivery arrives via `notifyMeshLoaded` →
   `LLVOVolume::notifyMeshLoaded` → `markRebuild` (llmeshrepository.cpp:4963-5002).
   Loaded-mesh short-circuit: a second object with the same UUID gets the cached
   system volume immediately (the "see what we can display while we wait" search,
   llmeshrepository.cpp:4516-4557), which is what makes R4 adoption free.
6. Transform from the record; TE fill once the mesh resolves (below).
7. Record TEs and the LOD pin applied; the object joins the world's `mObjects`
   (`std::vector<LLPointer<SSAtmoLandscapeObject>>` — the SSWaterWorld ownership
   pattern, which survives `gObjectList.killObject` marking the object dead while the
   pointer keeps the body alive for the dead-sweep).

Mesh faces arrive asynchronously: until `getVolume()->isMeshAssetLoaded()`, the object
is an invisible placeholder (`updateLOD` refuses via
`LLSculptIDSize::isUnloaded`, llvovolume.cpp:1847). On the first loaded frame the world
sizes the TE array — `LLViewerObject::setNumTEs(volume face count)`
(llviewerobject.cpp:5406, which fans into `LLPrimitive::setNumTEs`) — then applies the
record's faces and marks the record hydrated. This mirrors what the sim does for rezzed
meshes (processUpdateCore sizes TEs from the update); there is no server here, so
`SSAtmoLandscapeWorld::hydrateFaces()` does it once, keyed on
`mVolumeReady` (a cached bool on the object class).

`SSAtmoLandscapeObject` overrides:

- `getPartitionType()` → `LLViewerRegion::PARTITION_LANDSCAPE` (new; §7).
- `ssLODDistanceScale()` → `1.f / SS_LANDSCAPE_LOD_STRETCH` (R3; §6).
- `calcLOD()` → pinned-LOD freeze, else `LLVOVolume::calcLOD()` (R3; §6 — calcLOD is
  made virtual, llvovolume.cpp:1664, llvovolume.h:433).

Everything else — faces, draw pools, octree culling, fog, deferred passes, sun shadows,
texture streaming, `DebugObjectLODs` — is stock, unmodified, because the object is a
stock volume wearing a pcode the stock checks already recognise.

### 1.2 SSAtmoLandscapeWorld

Singleton, ticks once per frame from `llviewerdisplay.cpp` immediately after
`SSWaterWorld::update()` (llviewerdisplay.cpp:1000) — the same drive site, same frame,
so it reads the same resolved track the applier and water family read. One tagged call
line; no stock logic touched.

`update()` in order:

1. **Gate.** `want_active = SSAtmoEnabled && SSAtmoEnvManager::hasAsset() &&
   SSAtmoEnvApplier::isActive() && gAgent.getRegion() != NULL`. Inactive with a non-empty
   live set → kill everything (deselect first, see §10) and remember the cleared
   signature; return. Nothing exists while Atmo is off — the stock scene is byte-identical.
2. **Signature.** A `State` struct in the SSWaterWorld style (sswater.h:109-128):
   `{ bool mActive; U64 mAgentHandle; S32 mTrackIndex; U64 mRecordHash; }`, where
   `mRecordHash` folds the active track's record list (uuid, mode, position, rotation,
   scale, lod pin, enabled, face hashes, name/desc/provenance). A changed signature is
   "the record side changed": load, revert, track crossing, tab edit.
3. **Adoption** (`adoptRecords`, §2) — on signature change. This is R4 and R5's engine.
4. **Re-anchor** — on agent region handle change: `LLViewerObject::setRegion`
   (llviewerobject.cpp:7417) for every object (their region follows the agent's), then
   re-anchored placement for region-locked records (§6 note: setRegion's
   created-list bookkeeping no-ops on local id 0). Free objects keep their global
   position and only re-parent their region.
5. **Dead sweep** (`anyDead`, the SSWaterWorld::anyDead idiom, sswater.cpp:270-287) —
   region teardown/teleport kills objects out from under us; objects whose
   `isDead()` are recreated from their record silently.
6. **Write-back** (`writeBack`, §5) — the reconcile half: serialize live state, put()
   diff against last-sync, write changed fields into the record.

`SSAtmoEnvApplier` gains one published value (SS file, not stock diff): the resolved
primary track index. `apply()` already computes `blend.mPrimaryTrack` per frame
(ssatmoenvapplier.cpp:176-187); it stores it in `mPrimaryTrack` and exposes
`primaryTrackIndex()`. The landscape world reads the applier's index rather than calling
`SSAtmoEnvTrackResolver::resolve` a second time — one resolve call site per frame, and
the landscape can never disagree with the sky about which track is live. The applier's
existing `isActive()` is already published.

### 1.3 Ownership of state

- The **record** (`SSAtmoEnvTrack::mLandscape[i]`, held in the manager's working
  `SSAtmoEnvAsset` via `editable()`) is the single persisted truth. The floater edits
  it, the reconcile writes it, save serializes it — one struct, one truth.
- The **object** is the rendering and editing surface; it is derived state, owned by
  the world, killed and recreated freely.
- The **LLSelectNode** is the editor's state store for name/desc while selected
  (seeded at selection; §4). Name/desc never round-trip through the object (an
  `LLViewerObject` has no name field), so they ride node→record, not the reconcile diff.

## 2. Data flow

**Drop → record → hydration → render.** The author drags a full-perm mesh item from
inventory onto the Landscape tab (the same floater the rest of Atmo Magic lives in;
`SSLandscapeListCtrl::handleDragAndDrop`, modeled on `SSTextureListCtrl::handleDragAndDrop`
at ssfloatertexturelist.cpp:175, but accepting only `DAD_MESH`
— llviewerassettype.cpp:81 maps AT_MESH → DAD_MESH):

1. `getIsFullPerm()` gate (llviewerinventory.cpp:2482 — modify-ok & copy-ok &
   transfer-ok). Non-full-perm: `ACCEPT_NO`, tooltip, and
   `LLNotificationsUtil::add("GenericAlert", ...)` with the house-rejection pattern
   used by the notecard drop (ssfloateratmoenv.cpp:633) — no new notifications entry.
2. Metadata capture (R2): `name`/`desc` from `getName()/getDesc()`;
   `creator`/`last_owner` from `getPermissions()->getCreator()/getLastOwner()`;
   `created_at` from `getCreationDate()`. All from the dropped `LLInventoryItem` —
   the exact fields the General tab later shows, read-only.
3. Record appended to `mgr->editable().mTracks[active].mLandscape` (R7's round-trip
   starts here — it is a record, not a live object, until the signature flips).
4. Next frame: signature change → `adoptRecords` → object created, hydrated per §1.1.
   Fetch begins immediately; TEs sized and record textures applied on mesh arrival.

**Track crossing (R4).** Per frame the world diffs `applier->primaryTrackIndex()`
against last frame's. On a change — physical crossing, teleport, or sit-teleport, the
same events the applier's resolver already separates (`teleported` → instant cut,
ssatmoenvapplier.cpp:172-177) — `adoptRecords(new_index)` runs *this frame*, before
anything renders:

- Pair the next track's records against live objects **by asset UUID**, greedily in
  list order (R5's duplicate-mesh case falls out of order-pairing; surplus pairs kill
  or create).
- Same UUID → **keep the instance**: apply the new record's transform, scale,
  rotation, TEs, LOD pin. `setVolume` is not called; the loaded `LLVolume` stays, no
  re-download, no rebuild pop — consistent with the "a preset can't be interpolated"
  idiom: the swap is an instant cut, done by the time the frame draws.
- UUID only in the new track → create (fetch begins; if it was fetched on another
  track moments ago it is already cached — the volume manager dedups by params).
- UUID only in the old track → kill.
- Live set = the *primary* track's list, matching the bridge/precipitation and applier
  track semantics — a track's scenery is atmosphere for its band, nothing more.

**Region crossing.** Signature's `mAgentHandle` changes → setRegion + re-anchor
(locked) or re-parent (free), exactly one instance at a time, never per-region copies —
the deliberate inversion of the SSWater family, which builds per-region planes because
water must be per-region; scenery is authored per region *origin*, not per region
*copy* (R6).

**Load / unload of an environment.** `SSAtmoEnvManager::adoptParsedAsset` /
`applyNotecardText` replaces `mWorking` wholesale (ssatmoenvmanager.cpp:932-944) →
signature change → full adoption (same UUID-pairing path, so a load that keeps most
meshes mostly keeps their instances). `unload()` (ssatmoenvmanager.cpp:915) empties the
asset → gate fails → world kills everything. Nothing landscape-specific hooks the
manager; the signature is the only coupling.

## 3. Schema

`SSAtmoEnvTrack` gains `std::vector<SSAtmoLandscapeRecord> mLandscape;`
(ssatmoenvasset.h, SS file) and its `asLLSD`/`fromLLSD` gains the `"landscape"` key.
`SSAtmoLandscapeRecord` lives in `ssatmolandscape.h` (included by ssatmoenvasset.h);
round-trip follows the existing struct convention. Key is optional in `fromLLSD`
(missing → empty list), so no version bump: additive keys with defaults load from any
existing notecard, and the pretty-XML round-trip keeps hand-editing viable.

```xml
<key>landscape</key>
<array>
  <map>
    <key>asset_id</key><string>mesh asset uuid — the adoption key (R4)</string>
    <key>name</key><string>Alpine Ridge</string>            <!-- R2, editable -->
    <key>desc</key><string>seen from the east</string>      <!-- R2, editable -->
    <key>creator</key><string>…uuid…</string>               <!-- R2, read-only -->
    <key>last_owner</key><string>…uuid…</string>            <!-- R2, read-only -->
    <key>created_at</key><integer>1777334400</integer>      <!-- unix, from the item -->
    <key>enabled</key><boolean>true</boolean>
    <key>pos_mode</key><string>region</string>              <!-- "region" | "free" -->
    <key>local</key><array><real>96</real><real>128</real><real>0</real></array>
    <key>global</key><array><real>…</real><real>…</real><real>…</real></array> <!-- free only -->
    <key>rot</key><array><real>x</real><real>y</real><real>z</real><real>w</real></array>
    <key>scale</key><array><real>1</real><real>1</real><real>1</real></array>
    <key>lod_pin</key><integer>-1</integer>                  <!-- -1 auto, 0..3 pinned -->
    <key>faces</key><array>
      <map>
        <key>tex</key><string>texture asset uuid</string>
        <key>mat</key><string>pbr material uuid, empty if none</string>
        <key>color</key><array><real>r</real><real>g</real><real>b</real><real>a</real></array>
        <key>repeat</key><array><real>1</real><real>1</real></array>
        <key>offset</key><array><real>0</real><real>0</real></array>
        <key>trot</key><real>0</real>                        <!-- degrees -->
        <key>shiny</key><integer>0</integer>
        <key>bump</key><integer>0</integer>
        <key>fullbright</key><boolean>false</boolean>
        <key>glow</key><real>0</real>
      </map>
    </array>
  </map>
</array>
```

C++ mirror:

```cpp
struct SSAtmoLandscapeRecord
{
    LLUUID mAssetId;             // mesh asset UUID — the R4 adoption key
    std::string mName, mDesc;
    LLUUID mCreator, mLastOwner; // read-only provenance (R2)
    U64 mCreatedAt = 0;
    bool mEnabled = true;
    bool mRegionLocked = true;
    LLVector3 mLocalPos;         // locked: region-local metres, X/Y 0..width, Z free
    LLVector3d mGlobalPos;       // free: global metres
    LLQuaternion mRotation;
    LLVector3 mScale = LLVector3(1.f, 1.f, 1.f);
    S32 mLODPin = SS_LANDSCAPE_LOD_PIN_AUTO;   // -1, or 0..3
    struct Face { LLUUID mTexture, mMaterial; LLColor4 mColor;
                  LLVector2 mRepeat{1,1}, mOffset; F32 mRotDeg; U8 mBump, mShiny;
                  bool mFullbright; F32 mGlow; };
    std::vector<Face> mFaces;
    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);   // tolerant, like the other schema structs
};
```

Consts in ssatmolandscape.h: `SS_ATMOENV_MAX_LANDSCAPE = 64` (per track, soft cap;
drop beyond it is refused with a message), `SS_LANDSCAPE_LOD_STRETCH = 8.f`,
`SS_LANDSCAPE_LOD_PIN_AUTO = -1`.

The persisted TE set is the **face-panel authorable subset** — texture, PBR material id,
colour(+alpha), repeats/offset/rotation, shiny/bump/fullbright, glow — and deliberately
not the rest of `LLTextureEntry` (media, flexible data, particle slots are all
meaningless on a one-piece mesh scenery face). This is a cut, stated as one: the schema
persists what the editor can edit, not the wire format. Materials dropped onto faces
pass the same fullperm gate as the mesh (§4), so the record travels with the estate.

## 4. Editing UX

The editing surface is the **stock build tools**, because the object is a stock volume:
select in-world (click) → build floater; translate/rotate/scale with the stock manips
(`llmaniptranslate.cpp`/`llmaniprotate.cpp`/`llmanipscale.cpp` — all client-side local
transforms ending in `sendMultipleUpdate`, which is gated); face panel for
textures/materials (client-side `setTEImage` + gated `sendTEUpdate`);
texture/material drops on faces (`lltooldraganddrop.cpp` applies via
`setTEImage`/`setTEGLTFMaterialOverride` client-side before `sendTEUpdate` — the local
half survives the gate intact); `Inspect` reads the seeded node. The Landscape tab is a
*management* surface, not an editor: rows [mesh icon, name, mode chip], row actions
select-in-world (`LLSelectMgr::selectObjectAndFamily`), enable/disable, delete,
reorder (up/down), duplicate (copies the record, same mesh UUID — legal under R5),
region-locked/free toggle, LOD pin dropdown (Auto/0/1/2/3).

**Selection machinery on a local object — the full gating map.** A selectable local
object must never let a server send leave with its local id 0. Every path is gated:

- **All selection sends** funnel through `LLSelectMgr::sendListToRegions`
  (llselectmgr.cpp:5940-6145) — ObjectSelect (sendSelect, 5524),
  ObjectDeselect, `MultipleObjectUpdate` (sendMultipleUpdate, 4892 — the
  position/rotation/scale channel every manip tool uses, llmaniptranslate.cpp:1079 and
  friends), DeRezObject (confirmDelete, llselectmgr.cpp:4431), ObjectLink, ObjectBuy,
  ObjectDuplicateOnRay, ObjectPermissions, ObjectName, ObjectDescription. The
  push functors inside `sendListToRegions` skip nodes whose object
  `isLocalContent()`; a selection that is entirely local builds an empty queue and the
  function bails before touching `gMessageSystem` — **one gate covers every Object\*
  message in the viewer**. Stock behaviour for non-local nodes in a mixed selection is
  unchanged.
- **`requestObjectPropertiesFamily`** (llselectmgr.cpp:6152, called from selection at
  1216/1397) — early-returns for local content; the properties the panels read are
  seeded locally (below), so the request would only produce a warning lookup for a
  localID-0 object.
- **Per-object sends**: `sendTEUpdate` ("ObjectImage", llviewerobject.cpp:5528 — the
  face-texture drop tail at lltooldraganddrop.cpp:1235 calls it),
  `sendShapeUpdate` (5507), `sendMaterialUpdate` (5491), `updateFlags`
  ("ObjectFlagUpdate", 7474), `fetchInventoryFromServer` (task inventory requests,
  3228), and `send_ObjectGrab_message` (lltoolgrab.cpp:1185) all early-return for
  `isLocalContent()`. These gates are also what keeps *other* code paths (area search
  helpers, scripted UI) from firing localID-0 traffic at the sim.
- **Permission seams** — the object carries permission *flag bits* set at
  construction (permModify/permCopy/permMove read `mFlags`, llviewerobject.cpp:7259,
  7283, 7307), so every stock enable check (`selectDelete`'s permMove/permCopy/permYouOwner,
  General tab `objectp->permModify()`, face panel gating) reads true while the sends
  they would trigger are gated. Atmosphere has no server state to lose.

**Seeding `LLSelectNode`** (llselectmgr.cpp nodes are created at addAsFamily:1068,
addAsIndividual:1114, hover:1211): each creation site calls
`SSAtmoLandscapeWorld::seedSelectNode(nodep)` (a tagged one-liner behind an
`isLocalContent()` check). The seed fills, from the record: `mName`, `mDescription`
(the General tab's line editors read `nodep->mName`/`nodep->mDescription`,
llpanelpermissions.cpp:628/635), `mPermissions` (owner = agent, creator/last-owner =
R2's captured values, full base/owner masks — so `selectGetOwner`, creator display,
derez enable checks and Inspect all read a coherent object), `mValid = true`,
`mCreationDate` (the record's `created_at`), and `saveTextures` from the current TEs
(so the face panel's cancel-restore state is populated the way
`processObjectProperties` does it). `mValid = true` is what makes the node appear in
`valid_iterator` selections — without it the build floater treats the selection as
still-loading.

**Name/desc writes.** `LLPanelPermissions::onCommitName` →
`LLSelectMgr::selectionSetObjectName` ("ObjectName" via sendListToRegions,
llselectmgr.cpp:5245-5274). Gate behaviour: if the selection is entirely local, skip
the send, write `node->mName` (and `mDescription` twin) directly, and push into the
record via the world. Same for `selectionSetObjectDescription`. The General tab reads
back `nodep->mName` on its next refresh — the write lands where the read comes from.
Creator/last-owner render from the seeded permissions, read-only, exactly as the
server would show them (R2 made visible with zero new UI).

**Delete / Take.** Pie-menu Delete → `LLSelectMgr::selectDelete` (4356) →
`confirmDelete` → DeRezObject. In `confirmDelete` (llselectmgr.cpp:4414) a
tagged branch: selection entirely local → `SSAtmoLandscapeWorld::removeSelected()`
(deselect, kill objects, erase records) and return; the DeRezObject send never builds.
Mixed selections are filtered in `derez_objects` (llviewermenu.cpp:6324 —
the collection point all derez destinations flow through): local objects are pulled
from the derez list and their records removed; the rest derez normally. "Take" is the
same path with the same outcome — the atmosphere equivalent of taking an object is
deleting the entry; the mesh item itself is untouched in inventory.

**Undo/redo.** The stock viewer has no object-edit undo to converge with — there is no
undo stack and no ObjectUndo message in this codebase; stock edit "cancel" semantics
(deselect = nothing was sent yet) rest on the server being the only store. Here the
live object *is* the editor surface and the record follows it (§5), so the
editor-visible state and the persisted state are the same thing at all times. The
undo-shaped affordances are: drag back (the record follows within one reconcile pass),
the tab's per-row Delete, and the floater's existing Revert (`revertToBaseline` →
mWorking = mBaseline → signature change → `adoptRecords` re-applies the baseline
records to the live objects — a full round-trip with no landscape-specific code).

**Shape tab.** Inert on a mesh object in stock (path/profile params are meaningless for
`LL_SCULPT_TYPE_MESH`); its sends are gated (`sendShapeUpdate`) and the reconcile
snapshot captures only the sculpt id identity — a stray shape edit cannot corrupt the
record's mesh identity. Contents tab: inventory requests are gated
(`fetchInventoryFromServer`), so the tab shows an empty list rather than a server error.

## 5. Persistence

**Reconcile loop, not event hooks** — the put() cache-and-diff pattern generalized to a
two-directional loop (ssatmoenvapplier.cpp:545-553 is the precedent: cache, compare,
write only on change).

Per live object, the world keeps a `SSLandscapeSync` snapshot — POD-ish: mode,
position (region-local for locked, global for free), rotation, scale, per-face
(texture, material, colour, repeat, offset, rot, bump, shiny, fullbright, glow), TE
count. `writeBack()` per frame:

```cpp
// ssatmolandscape.cpp — the put() shape, generalized
auto put = [&dirty](auto& cache, const auto& value, auto&& setter)
{
    if (!(cache == value)) { cache = value; setter(value); }
};
```

- Live state reads straight from the object: `getPositionLocal()`/`getPositionGlobal()`
  (per mode), `getRotation()`, `getScale()`, `getTE(i)` fields
  (`getID`, `getColor`, repeats/offsets/rotation, bump/shiny/fullbright, glow) and
  `getRenderMaterialID(i)`.
- On a diff → write the fields into the record held in `mgr->editable()`. The working
  asset is the live document the floater reads and `saveNotecard` serializes, so R7's
  "existing manager round-trip" is literally the existing one.
- Every editor path converges here: manip tools (client-side transforms),
  face-panel commits (`setTE*` client-side + gated send), texture drops
  (`setTEImage` before the gated `sendTEUpdate`), LOD pin (record-side), name/desc
  (node→record at the two injection points). No event hooks, no "which editor wrote
  this" dispatch — the live object *is* the event.

The reverse direction — records arriving from outside the loop (load, revert,
track crossing, tab edits) — is **adoption** (`adoptRecords`, §2), keyed on the
per-frame signature. Conflict rule, stated once and held everywhere: **the live object
wins for any field it was edited in this pass; the record wins otherwise.** An author
mid-drag during a revert keeps their drag; everything idle adopts the reverted records.
Deterministic, and the mid-edit case degrades to "re-revert after deselect" rather
than fighting the editor.

Load restores by the same machinery: `adoptParsedAsset` → signature → adoption with
UUID pairing. A restore that keeps the same mesh keeps the instance and its loaded
LODs; only authored fields move.

## 6. Region-locked vs free positioning (R6)

`mRegionLocked` per record. Exactly one live instance per record, ever — re-anchored,
not copied (the SSWater family builds per-region planes because water is per-region
state; scenery is a single authored thing whose *anchor* is per region).

- **Locked**: the record stores a region-local offset (`mLocalPos`, X/Y clamped
  0..region width, Z free — Z is the shared sea-level datum, exactly like the water
  plane's height semantics). On agent region change (signature's `mAgentHandle`):
  `vo->setRegion(gAgent.getRegion())`, then
  `setPositionGlobal(regionp->getOriginGlobal() + mLocalPos)`. The build floater's
  position fields show region-local coordinates already, so **what you author is what
  every region gets** with no new editor surface: the stock readout is the record.
- **Free**: global position; on region change the object only re-parents
  (`setRegion`) and stays where it is in global space.

The re-anchor is *before* the write-back diff in the frame order, and the snapshot
serializer works in the record's own frame (local for locked, global for free), so a
re-anchor is not an edit — the diff stays quiet. The dead sweep is what recovers from
teleports (the old region's `killObjects` kills the instance; the sweep recreates from
the record before the frame draws — a teleport is a cut anyway).

## 6b. LOD strategy (R3) — the exact minimal core change

`LLVOVolume::calcLOD` (llvovolume.cpp:1664) is the anchor; `updateLOD` is already
virtual (llvovolume.h:247), `calcLOD` is not (llvovolume.h:433). Two tagged edits in
`llvovolume.h`, one line in `llvovolume.cpp`:

1. `bool calcLOD();` → `virtual bool calcLOD();` — one word.
2. `virtual F32 ssLODDistanceScale() const { return 1.f; }` — the hook.
3. In `calcLOD`: `distance *= sDistanceFactor;` →
   `distance *= sDistanceFactor * ssLODDistanceScale();` (llvovolume.cpp:1755).

`SSAtmoLandscapeObject` overrides both:

- `ssLODDistanceScale()` returns `1.f / SS_LANDSCAPE_LOD_STRETCH` (8×). The LOD math is
  single-sourced — virtualizing the whole function would fork ~160 lines of LOD physics
  (rigged branch, near-boost ramp, FOV-zoom factor, the `DebugObjectLODs` overlay path)
  that upstream keeps touching, and every one of them stays shared here. Multiplying
  the *adjusted* distance by 1/8 stretches every switching distance 8× — the stock
  0-256 m curve becomes 0-2048 m — while `mLODDistance` keeps the true distance, so the
  LOD debug overlay still reads reality. The Mesh-detail preference
  (`sLODFactor`/`RenderVolumeLODFactor`) still feeds `lod_factor` unchanged, and the
  FOV-zoom factor applies as before. The near-boost ramp (`distance < rampDist`)
  widens 8× with the scale — closer geometry reaches full detail sooner, which is what
  the ramp is for. `mLODScaleBias × getScale()` still feeds the radius term, so a
  500 m mountain shows high LOD farther out than a 5 m rock *in addition to* the
  stretch — radius scaling is per-object and automatic.
- `calcLOD()` override: when `mLODPin >= 0`, apply the pin once
  (`if (mLOD != mLODPin) forceLOD(mLODPin);` — `forceLOD` exists, llvovolume.cpp:1828,
  FIRE-21445 — guarded so it fires once per pin change) and return false; else
  `return LLVOVolume::calcLOD();`. The stock `DebugSelectionLODs` pin path keeps
  working under it, because it lives inside the base function we delegate to.

Stock objects are untouched by construction: the hook's base implementation is the
constant 1.0, and only our subclass overrides it. No stock object's LOD changes with
Atmo on or off.

## 7. Culling / draw distance (R7)

The landscape partition is the whole story. `SSLandscapePartition : LLVolumePartition`
sets `mInfiniteFarClip = true` (the `LLWaterPartition` precedent, llvowater.cpp:295-301)
and keeps `mDrawableType = RENDER_TYPE_VOLUME`, so:

- `LLSpatialPartition::cull` takes the `LLOctreeCullNoFarClip` branch
  (llspatialpartition.cpp:1469-1473) — frustum-only culling, ignoring the
  draw-distance far plane exactly as water does. The hard edge is the constant
  projection far plane (`MAX_FAR_CLIP`; the main-view projection overrides the
  draw-distance far — llviewercamera.cpp:301-306 + 352-356, and
  llviewerdisplay.cpp:240-267's far is only used for culling), which is the same
  "the projection far plane, not the draw distance" semantics the water ring renders
  under. 0-2048 m effective range and the 8× LOD stretch agree with each other by
  construction.
- Occlusion stays on (a mountain is real occluder geometry); draw distance does not
  gate it.
- Every pipeline pass that enumerates partitions does so generically
  (`for (U32 i = 0; i < LLViewerRegion::NUM_PARTITIONS; i++)` keyed on
  `hasRenderType(part->mDrawableType)` — render cull at pipeline.cpp:2692, shadow
  culling, restoreGL at 1709), so a new partition slot is picked up everywhere
  without per-site changes. Drawables render in the standard opaque/deferred path:
  no new pool, no shader, no render-list ordering.
- Known limits, named: partition lists that are explicitly enumerated rather than
  enumerated — `lineSegmentIntersectWorldGeometry`'s `world_partitions`
  (pipeline.cpp:7507-7512, OTS shoulder camera collision) and the 7352 collision
  list — do not include the landscape partition, so the shoulder cam can clip through
  scenery. Deliberately not fixed: expanding those lists touches gameplay-adjacent
  code for a cosmetic edge case.

Atmo-off behaviour is trivial: the partition exists but holds nothing.

## 8. Failure modes

- **Non-fullperm drop** — refused at the tab (`getIsFullPerm`), tooltip + generic
  alert. The gate doubles as the fetch gate: `getProtectedAssetUUID`
  (llviewerinventory.cpp:2464-2480) returns null for non-fullperm items, so a rejected
  drop is also the only record that could ever have failed to fetch.
- **Mesh 404 / purged asset**: `LLVOVolume::setVolume`'s `is404` path
  (llvovolume.cpp:1247-1251, 1283-1289) nulls the sculpt id and renders the stock
  failed-mesh proxy (box + Inv_Mesh icon). The world detects a null sculpt id on a
  hydrated object, keeps the record, flags the row in the tab ("mesh unavailable"),
  and leaves the stock proxy in place — the same thing every player sees for a purged
  rezzed mesh. A later `Toggle` (enable/disable) or re-drop retries.
- **Fetch stalls (no caps, empty region)**: `loadMesh` requests stall in pending until
  caps arrive — identical to a rezzed mesh. The world gates hydration on
  `gAgent.getRegion()->capabilitiesReceived()` so nothing half-initializes before the
  caps exist.
- **Hand-edited notecard with a null/invalid asset id**: record skipped at adoption
  with a console warning; row shows "invalid" in the tab. No crash, no orphan object.
- **Track resolver inactive**: resolver is a pure static; its only inputs are the asset
  and the camera Z. "Inactive" reduces to the applier gate (off / no asset) — the world
  kills everything and stock behaviour resumes. A track carrying landscape records that
  is never the active track simply never renders them — correct, not a failure.
- **Selected object killed out from under the editor** (toggle off, teleport, delete):
  the world deselects before killing (`LLSelectMgr::deselectObjectAndFamily(obj,
  false)`), the build floater closes or idles on stock dead-node handling — the same
  path a region teardown already exercises for selected objects.
- **Mixed local/stock selection**: delete splits (records removed, stock derez
  proceeds); manipulators move what they can; sends are per-node filtered so stock
  objects keep functioning.
- **Teleport**: region teardown kills objects → dead sweep recreates from the record
  before render. Visually identical to any teleport cut; adoption signature (agent
  handle) triggers re-anchor on the next frame.
- **Face count mismatch after mesh swap**: a re-dragged mesh with a different submesh
  count re-sizes TEs at hydration; record faces beyond the new count are dropped, new
  faces default — stated, predictable, editable.

## 9. Performance

- **Per-frame idle**: one `update()` — a signature fold over the active track's records
  (≤64 records × ~15 scalar fields) plus the write-back pass over ≤64 live objects
  (snapshot struct compare, ~300 bytes each). Compare-only until a diff exists; LLSD
  serialization happens only at save time. Sub-millisecond for the designed scale; the
  LLSD conversion cost is paid on save, never per frame (the put() precedent's exact
  property).
- **LOD**: the stretched distance factor is one multiply in the hot path of our objects
  only; stock volumes pay one extra virtual call per `calcLOD` — measured in the noise
  and only when the pipeline calls `updateLOD`.
- **Memory**: per object, an `LLVOVolume` + drawable + faces (a rezzed mesh's cost) and
  a per-object snapshot struct; meshes are shared through the system volume manager and
  `LLSculptIDSize` refcounting, so N landscape objects over the same mesh cost one
  volume. Textures likewise shared. The honest worst case is the meshes themselves —
  which is the feature's payload, not overhead.
- **Worst case**: 8 tracks × 64 records serialized is a few hundred small maps in the
  notecard; only **one** track's objects ever live simultaneously. The octree cost is
  ≤64 drawables in one region's landscape partition.
- **The reconcile writes at most one record per changed object per frame** — a full
  drag at 60 fps writes 60 record updates per second to in-memory structs; the LLSD
  serialization is only ever on explicit save. No LLSD is built per frame.

## 10. Opt-in / off behaviour

- **`SSAtmoEnabled` off** (or no asset loaded, or the applier inactive): the gate in
  §1.2 kills everything the first frame; records stay in the asset untouched. Nothing
  runs, nothing renders, no stock path changes — the LOD hook, the partition, and the
  gates are inert for stock objects by construction (the flag is only ever set by our
  constructor). Re-enabling: adoption runs, UUID matching reuses what it can.
- **Environment unload / parcel-discovery loss**: `manager->unload()` empties the
  asset — same gate — objects die, selection is dropped first, stock EEP returns. The
  Bridge/parcel-discovery machinery is untouched; a re-discovered asset re-adopts with
  the same UUID pairing (mesh caches make it cheap).
- **Leaving a configured parcel** follows the applier's own deactivation logic — the
  landscape world has no parcel observer of its own and needs none.
- **Logout / world teardown**: `LLWorld::resetClass` kills the live set before
  `gObjectList.destroy()` (the `SSWaterWorld::clearWaterObjects` precedent,
  llworld.cpp:134-139), so no `LLPointer` outlives teardown holding a dead drawable.

## 11. Stock file diff list

Every injection is wrapped in `<SS:Nexii>` … `</SS:Nexii>`; all are early-out gates,
one-word virtualizations, or one-line calls — none changes stock behaviour for stock
objects. `SS` prefix on every new identifier; no `FS` names.

| Stock file | Injection |
|---|---|
| `indra/newview/llviewerobject.h` | `<SS:Nexii>` member block on `LLViewerObject`: `bool mIsSSLocalContent = false;` + `bool isLocalContent() const` + `void setLocalContent(bool)` — the flag every gate reads |
| `indra/newview/llviewerobject.cpp` | Five gates, each an early `if (isLocalContent()) return;` at the top of: `sendTEUpdate` (5528, "ObjectImage" — the texture-drop and face-panel channel), `sendShapeUpdate` (5507), `sendMaterialUpdate` (5491), `updateFlags` (7474, "ObjectFlagUpdate"), `fetchInventoryFromServer` (3228, task-inventory request) |
| `indra/newview/llselectmgr.cpp` | 1) `sendListToRegions` (5940): the local `push_all`/`push_some` functors skip nodes whose object `isLocalContent()` — gates ObjectSelect/Deselect, MultipleObjectUpdate, DeRezObject, ObjectLink, ObjectName/Description, ObjectBuy, ObjectPermissions, ObjectDuplicateOnRay in one place; 2) `selectionSetObjectName` (5245) / `selectionSetObjectDescription` (5276): all-local branch writes `node->mName`/`node->mDescription` + record instead of sending; 3) `requestObjectPropertiesFamily` (6152): early return for local content; 4) `addAsFamily` (1068), `addAsIndividual` (1114), hover-node creation (1211): `SSAtmoLandscapeWorld::seedSelectNode(nodep)` behind an `isLocalContent()` check; 5) `confirmDelete` (4414): all-local selection → `SSAtmoLandscapeWorld::removeSelected()`, DeRezObject send skipped |
| `indra/newview/llvovolume.h` | `bool calcLOD();` → `virtual bool calcLOD();` (one word, llvovolume.h:433); add `virtual F32 ssLODDistanceScale() const { return 1.f; }` — the R3 hook |
| `indra/newview/llvovolume.cpp` | `calcLOD` (1664): `distance *= sDistanceFactor;` → `distance *= sDistanceFactor * ssLODDistanceScale();` (1755) |
| `indra/newview/llviewerregion.h` | `PARTITION_LANDSCAPE` inserted after `PARTITION_VOLUME` in the `eObjectPartitions` enum (99) |
| `indra/newview/llviewerregion.cpp` | `#include "ssatmolandscape.h"`; `initPartitions` (716): one `mImpl->mObjectPartition.push_back(new SSLandscapePartition(this));` after PARTITION_VOLUME's line, order kept matching the enum |
| `indra/newview/llviewerdisplay.cpp` | `SSAtmoLandscapeWorld::getInstance()->update();` in the tagged Atmo block after `SSWaterWorld::update()` (1000) |
| `indra/newview/llworld.cpp` | `SSAtmoLandscapeWorld::getInstance()->killAll();` in `resetClass` (134), beside the SSWaterWorld clear |
| `indra/newview/lltooldraganddrop.cpp` | `dropTexture` (1592), `dropMaterial` (1238), `dropMesh` (1568): `if (SSAtmoLandscapeWorld::gateFaceDrop(hit_obj, item)) return;` — fullperm gate for face drops on landscape objects (R1's consistency rule), and refusal of mesh drops *onto* the object (mesh identity lives in the record; re-drag on the tab row swaps it) |
| `indra/newview/lltoolgrab.cpp` | `send_ObjectGrab_message` (1185): early return for local content — Touch/Grab on atmosphere sends nothing |
| `indra/newview/llviewermenu.cpp` | `derez_objects` (6324): local-only selections → `SSAtmoLandscapeWorld::removeSelected()`; mixed selections filtered (locals removed from the derez list, their records removed) |
| `indra/newview/CMakeLists.txt` | `ssatmolandscape.cpp`, `ssatmolandscapeui.cpp` + headers appended to the viewer file lists |

SS-owned file changes (not stock diffs, listed for completeness): `ssatmoenvasset.h/.cpp`
— `#include "ssatmolandscape.h"`, `std::vector<SSAtmoLandscapeRecord> mLandscape` on
`SSAtmoEnvTrack`, the `"landscape"` key in its round-trip; `ssatmoenvapplier.h/.cpp` —
publish `primaryTrackIndex()`; `ssfloateratmoenv` + `floater_ss_atmo_env.xml` — the
Landscape tab panel. New files: `ssatmolandscape.h/.cpp`,
`ssatmolandscapeui.h/.cpp`, `panel_ss_atmo_landscape.xml`. No new settings: the feature
is gated end to end on the existing `SSAtmoEnabled`.

## Known risks (stated, not hidden)

- **The selection gates are the load-bearing wall.** `sendListToRegions` covers every
  Object\* message funnelled through the selection manager today; any *new* send path
  added upstream that bypasses it (like FS's `requestObjectPropertiesViaSelect`, which
  FSAreaSearch bypasses with direct sends) can leak localID-0 traffic. The gate list is
  honest about this class of hole but cannot be closed once and forgotten.
- **`isLocalContent()` on `LLViewerObject` is one bool away from habit.** Anything that
  later constructs adopted objects and forgets to set the flag falls back to sending
  — the failure mode is noisy (server rejects localID 0), not silent.
- **Adoption pairing is greedy by list order.** Two records sharing a mesh UUID across
  tracks where one track has one slot and the other has two loses an instance across
  the boundary (a re-fetch, not a crash). Acceptable for scenery lists.
- **Mid-edit revert loses the in-flight edit for dirty objects** (§5's conflict rule).
  Predictable, documented, and better than the reverse.
- **Un-gated per-packet property probes** (FSAreaSearch's direct property requests,
  radar-style listings) show landscape objects as empty rows with localID 0. Cosmetic;
  not worth further stock diff.
