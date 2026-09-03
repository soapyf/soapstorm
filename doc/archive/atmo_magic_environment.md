# Atmo Magic: a unified, opt-in environment system

> **OUT OF DATE** — archived 2026-08-26. Kept for historical reference; the code is the ground truth.

This started as a design proposal (internally coded "v3" while it sat next to
the v1/v2 code it was superseding); it's now the live system, and this
document tracks it as such. It specifies a fully client-side, opt-in
environment system that carries the Atmo Magic branding forward as a
**complete, self-contained renderer**, cleanly separated from stock v2
(EEP/Windlight) rather than living inside it.

## Relationship to what exists

- **v1** — Windlight: one flat, scrolling-noise cloud plane, one sky/water/day
  asset set per region.
- **v2** — **stock EEP**, upstream LL behaviour: region sky/water/day assets,
  up to four altitude-keyed sky tracks via
  `LLEnvironment::calculateSkyTrackForAltitude`. This is not Soapstorm's own
  system — it's Linden's, and it stays exactly as upstream ships it.
- **Atmo Magic Weather (legacy)** — the original weather layer (see
  [`atmo_magic_tracks.md`](atmo_magic_tracks.md)), built by injecting into and
  modifying v2 in place, riding its four altitude tracks rather than having
  its own. Deprecated now that Atmo Magic (below) covers the same ground with
  its own asset and its own notion of "track" — kept around, trimmed to its
  still-useful parts (the precipitation preset editor, the simulation
  floater), as a reference point for comparing feature-completeness against
  while Atmo Magic catches up. `SSAtmoTrackMgr`/`SSAtmoTrackConfig`/
  `ssfloateratmo.*` are its classes; `SSAtmoMagic`/`ssatmomagic.*` is the
  shared particle/wind/rain renderer both this and Atmo Magic proper drive,
  and isn't part of the deprecation - see `SSAtmoEnvBridge` below for how the
  two connect.
- **Atmo Magic** (this document) — the branding's primary system: a wholly
  independent, fully parallel renderer with its own unified asset and its own
  notion of "track", decoupled entirely from `LLEnvironment`. Classes/files
  use an `Env` token (`SSAtmoEnvAsset`, `ssatmoenvmanager.cpp`,
  `floater_ss_atmo_env.xml`, ...) to stay distinct from `SSAtmoMagic` the
  renderer above, which is a different thing feeding off both this and the
  legacy layer via `SSAtmoEnvBridge`.

**How it got here, for the archaeology:**
1. **Fork** — the original weather layer's (`ss`-prefixed) logic was
   duplicated into independent files, decoupled from `LLEnvironment`
   internals to run standalone.
2. **Untangle** — `git diff master...feature-atmo-magic` on every EEP-proper
   file (`llenvironment.*`, `llsettingsvo.*`, `llsettings{sky,water,day,base}.*`,
   `llpaneleditsky.cpp`, `llpaneleditwater.cpp`, `llfloaterenvironmentadjust.cpp`,
   `lllegacyatmospherics.*`, `llvosky.*`) comes back **empty** — v2/EEP is
   pristine, confirmed by both the diff and an absence of any
   `<SS:Nexii>`-tagged comment in those files. There is nothing to revert on
   the EEP side. The actual injection lives in roughly twenty core-engine
   files instead — `pipeline.cpp/h`, `llviewerdisplay.cpp`, `llagentcamera.cpp`,
   `llworld.cpp`, `llflexibleobject.cpp`, `llviewerpartsim.cpp`,
   `llviewerobject(list).cpp/h`, `llviewermessage.cpp`, `llvowater.cpp`,
   `lldrawpoolwater.cpp`, `lldrawpoolwlsky.cpp`, `llvoavatar.cpp/h`,
   `llviewershadermgr.cpp/h`, `llviewermenu.cpp`, `llappviewer.cpp`,
   `llviewerwindow.cpp`, `llviewercamera.h`, `llviewerfloaterreg.cpp`,
   `fsrezqueue.cpp/h` — each `<SS:Nexii>`-tagged hook reviewed in place rather
   than diffed against an upstream baseline.
3. **Diverge** — the fork evolved independently from there, into everything
   else this document specifies (multi-track, planetary system, per-param
   keyframes, the weather cube, and so on).
4. **Rename** — once the fork was functional enough to be the primary system
   rather than a parallel experiment, every class/file/floater dropped its
   `V3`/`v3` scaffolding name in one pass, replaced with the `Env` token where
   a plain `SSAtmoMagic`-style name would have collided with the still-live
   renderer class of that name. The legacy v1/v2 authoring layer was
   deprecated and trimmed in the same pass rather than deleted outright - see
   above.

## Storage & discovery

- **Format:** LLSD (XML), matching the existing weather layer's notecard
  format and reusing its (de)serialization machinery rather than inventing a
  second one.
- **One asset, no tracks-as-separate-assets:** the whole environment — every
  track's sky/water/weather/planetary config, all keyframes — is one
  document.
- **Parcel discovery:** reuses the same `atmo:<uuid>` marker convention
  already used by the v2 weather layer, resolving to an Atmo Magic unified
  asset instead. **Needs a disambiguator** — see Open Items.
- **Fetch protocol: a plain HTTP round trip through the existing Bridge
  plumbing, not a chunked chat relay.** An earlier pass in this doc
  specified `llOwnerSay`-chunked chat tags (`<Notecard:UUID:index.total>`);
  that turned out to be solving a problem the Bridge doesn't actually have.
  The Bridge already runs an HTTP channel for every other command
  (`FSLSLBridge::viewerToLSL` → `llRequestSecureURL`/`llHTTPResponse`, with
  `FSLSLBridgeRequestResponder`-style async callbacks on the viewer side),
  and `llHTTPResponse`'s body isn't chat-length-limited the way
  `llOwnerSay` is — so there's no chunking problem to solve at all once the
  right transport is used.
  - **LSL side:** a `FetchNotecard|<uuid>` command
    (see `indra/newview/fs_resources/EBEDD1D2-...-D47BBCA5DFB.lsltxt`) reads
    the notecard synchronously (`llGetNotecardLineSync`, NAK-sleep-retry, a
    3-second timeout), then replies over `llHTTPResponse` with the body
    passed straight through when it already starts with `<llsd>` — true for
    every Atmo Magic asset, since that's exactly what `LLSDSerialize::toPrettyXML`
    emits — or wrapped as `<llsd><string>...</string></llsd>` otherwise.
  - **Viewer side:** `SSAtmoEnvDiscoveryManager::requestFetch()` calls
    `FSLSLBridge::instance().viewerToLSL("FetchNotecard|" + uuid, callback)`.
    The HTTP layer already parses the `<llsd>` response body before the
    callback ever sees it, so an Atmo Magic asset's own content arrives as a real
    LLSD map directly — no text reassembly step of any kind. A response
    that isn't a map (the wrapped-string fallback path, or a hand-edited
    notecard) falls back to parsing it as LLSD-XML/notation text instead.
  - The UTF-8/chunk-size concerns this section used to carry (byte budgets,
    character-vs-byte counting) were specific to chat-based chunking and no
    longer apply — an HTTP body has no such limit to work around.
- **Manual save/load:** the floater gets **Save to Notecard** (writes a plain
  inventory notecard) and **Load from Notecard** (inventory picker +
  drag-and-drop onto the floater). No new asset type, no scripted delivery
  required for this path.
- **Creation has two entry points, same underlying action, no floater-side
  default.** There is no implicit environment that springs into existence
  just from opening the floater, and no "Defaults" button that resets to
  some standing state — Atmo Magic is opt-in end to end, so nothing runs until
  something has actually been created or loaded:
  1. **Inventory context menu.** The existing **New Settings** submenu
     (`menu_inventory_add.xml`, and its twin in the My Environments floater's
     `menu_settings_add.xml`) already holds `New Sky` / `New Water` /
     `New Day Cycle`, each a `menu_item_call` firing `Inventory.DoCreate`
     with a parameter string through `LLPanelMainInventory::doCreate()` →
     `menu_create_inventory_item(...)`. **New Atmo Magic** is a fourth entry
     in that same submenu, taking the same path except its creation branch
     writes a plain Notecard pre-filled with the default Atmo Magic LLSD body,
     instead of following Sky/Water/Day Cycle down the real
     Settings-asset-creation path. Kept because it's the discoverable,
     inventory-native way to start one — important on its own UX merits, not
     just as a fallback.
  2. **Floater's own landing state.** When the floater has nothing loaded,
     it shows a landing panel in place of the normal editing UI, with copy
     describing the drops it takes plus two one-click seeds: **Create Empty
     Environment** (the midday defaults, written straight away) and **Create
     Stock Day Cycle** (the shipped Daylight/Night/Sunrise/Sunset four-sky
     cycle). The one-time chooser that used to sit between the button and
     the seed is gone - every path acts immediately. A **drag directly on the
     floater** is the primary way in, and a drop does the whole job on its
     own: a notecard loads, a lone EEP day cycle or water preset seeds a
     new environment, and a group of EEP skies becomes a day cycle. All of
     the async fetch-and-seed paths run under a busy state that stands the
     floater down until the environment is ready, so nothing can be poked
     while it is half-built. This exists purely so opening the floater with
     nothing active doesn't dead-end into an empty panel with no visible way
     forward — it's a bootstrap affordance, not an ongoing control, and it
     disappears once something is loaded (at that point Save/Save As/Load/
     Revert are the relevant buttons, not this one).

  Either path produces the same thing: a real, inert notecard in inventory,
  pre-filled with the default Atmo Magic LLSD body, doing nothing until it's loaded.
- **Seeded from EEP, importable from EEP.** A new environment doesn't start
  from `LLSettingsSky`'s code-baked legacy numbers: creation fetches the four
  stock non-legacy skies concurrently and keyframes each into the ground
  track's Atmosphere & Lighting and Sky Dome at its canonical phase
  (Midnight 0.0, Sunrise 0.25, Midday 0.5, Sunset 0.75), then collapses any
  field all four agree on back to a plain value so constants don't carry
  redundant keyframes. Fetch failures degrade gracefully — whatever subset
  arrives is keyframed at its phases, a single survivor plain-seeds the
  whole track, none at all falls back to built-in defaults. Separately,
  dropping a **full-perm EEP sky settings item** onto the floater (once an
  environment is loaded) stamps that sky's look as keyframes at the current
  preview head into the selected track: scrub to a time, drop a sky, that
  instant now looks like that sky — repeat at other phases to build a day
  from existing EEP presets. Non-full-perm items are refused with an alert.
  A **water preset** dropped while an environment is loaded stamps the
  whole water block onto the selected track directly (and enables its
  plane); dropped with nothing loaded, it seeds the empty environment with
  that block. A lone **day cycle** dropped while an environment is loaded is
  refused — it would have to replace the loaded work — but with nothing
  loaded it translates into a new environment at its authored keyframe
  times. A **drop of several skies at once** while an environment is loaded
  stamps a day cycle across the selected track: measured phases, whole
  sky, no grouping dialog, the loaded counterpart of creating from skies.
- **Creation from skies drops directly** - what the chooser's "From skies I
  choose" mode used to gate behind a Create click is now just the drop. It
  accepts multiple sky drops at once (a whole inventory selection in one
  gesture) and places each supplied sky
  by the **actual elevation** of its dominant body: the phase where the
  track's reference body (sun or moon) reaches the height the sky drew it.
  The sky's **inventory name** sorts the placement to the right side of
  noon and names the fixed anchors — Sunrise (0.25), Sunset (0.75), Noon
  and Midnight pin their phases outright, **"Daylight" implies Noon** and
  **"Night" implies Midnight** (a night sky is never parked in daylight
  even when it carries no moon), and Morning/Evening/Dawn/Dusk claim the
  rising/setting side. Sunrise, Sunset, Noon and Midnight skies also
  supply the height their sun was drawn at, and together they **fit the
  pack's own sun path** — the daily elevation curve
  `sin(alt) = a − b·cos(2π·phase)`, whose midpoint comes straight from the
  horizon skies and whose half-day swing comes from the noon/midnight
  pair. Every other sky then lands at the root of that fitted curve on
  its named side, so no planetary tilt or latitude ever has to be
  guessed: the phases in the planetary system are the curve's roots. The
  fit is only trusted when the anchored skies agree (a "sunrise" sun far
  from the curve's midpoint rejects it); otherwise the default arc stands
  in. A "Morning Umbra" with its sun below the horizon lands in the
  pre-dawn hours, not in daylight. A sky without a name claim keeps the
  unique azimuth-derived measurement, and a night sky whose author parked
  the sun deep below the horizon (out of sight) while moving its moon is
  still placed by that moon, the visible body. The single sun and moon
  bodies are translated from the faces that matter: the sun disc from the
  sky where the sun stands highest (the day's face), the moon disc from
  the sky where the moon stands highest (the night's face). Creating from
  an EEP day cycle adopts the same two faces from the cycle's own
  keyframes. Every adopted disc texture gets its padding auto-derived from
  the alpha (the tightened 85%-solid-edge rule), exactly as a texture pick
  in the planetary designer does. The body
  diameters store the VISIBLE disc's physics: the sky's angular scale is
  authored against EEP's quad (a glow-boarded pre-padding sun is huge
  because its baked-in glow fills the quad), so once the padding lands the
  diameter rescales by the disc fraction - quad x (1 - 2 x padding) - and
  a 10.985-quad sun with 0.43 padding reads a ~1.5 solar-diameter body.
  The renderer draws the quad via the uncompressed distance divided by the
  perception scale, so the perception dials never leak into the authored
  world.
- The stock four-sky day cycle is itself just these anchors: Daylight noon,
  Night midnight, Sunrise and Sunset at the horizon — so the shipped seed
  and a user's "Daylight/Night/Sunrise/Sunset" pack place identically.
- **Auto-apply on parcel entry**, unless the Atmo Magic floater is currently open
  (treated as "mid-edit, don't clobber").

## Rendering model

- Fully parallel to `LLEnvironment` — borrows heavily from it rather than
  starting from scratch, but does not plug into its personal-override slot.
- A master enable switch gates everything, mirroring the v2 layer's
  `SSAtmoEnabled`: turning Atmo Magic support on doesn't load an environment by
  itself.

## Tracks

- **1 mandatory (ground) + up to 3 optional — 4 total**, deliberately
  matching the existing track count, but **not the same tracks**: Atmo
  Magic's track thresholds are freely author-defined, entirely decoupled from the region's
  actual EEP altitude-track settings. A skybox city at 900m can declare
  itself "ground zero" for its own isolated biome regardless of what the
  terrain below is doing.
- **Each track is a complete, isolated environment** — its own Atmosphere &
  Lighting, Clouds, Planetary, and Weather configuration, in full. Nothing
  blends *across* tracks; a Mars biome at altitude and an arctic ground track
  share nothing but the notecard they're saved in.
- **Activation:** the avatar's actual world Z crossing a track's configured
  threshold switches which track is active.
- **Transitions:**
  - Physically crossing a boundary → soft cross-fade over a configurable
    buffer zone (default ~10–20m).
  - Arriving via teleport, sit-teleport, or region position change → instant
    cut.
  - Water involved in the crossing → always instant cut, regardless of how
    it was entered.

## Water

- One optional water plane per track: enabled/disabled, keyframable height
  (tides), independent of that track's own altitude-activation band.
- Only one water plane is ever globally visible: whichever *enabled* track
  sits lowest — regardless of which track is currently active for the
  avatar, so a high skybox can still see the sea far below it.
- **Exclusion volumes:** any simple, non-hollow, non-deformed prim (same
  shape constraints as reflection probes) with "Hide Water" set on a face
  acts as one — camera-inside suppresses the underwater state
  (breathing/audio/post-process) without killing water fog or visibility
  where the surface is still in view, so underwater tunnels/domes work.

### Water off means no water

Disabling a track's water plane removes the water, not just its settings.
EEP water describes how water looks and never whether there is any, so the
applier switches the water render type off for the duration (the same
control, and the same "only undo what we turned off" bookkeeping, that
FSAllowEEPWaterDerender uses) and restores it when a water track becomes
active or Atmo Magic releases the slot. In the editor the whole Water tab
below the checkbox greys out, keyframe clusters included, and its rows
stop ghosting - the same treatment an Auto-owned row gets, for the same
reason. The authored values stay in the asset untouched, so turning water
back on restores the plane the author had rather than a default one.

## Weather tab

Per-track. Only simulated for whichever track the camera currently occupies
— no bleed-through from neighboring tracks.

**Inputs:** Moisture `[0,1]`, Convection `[0,1]`, Temperature
`[-30°C, +40°C]`, Wind Heading `[0°,360°)`, Wind Speed, Gust (depth/length/veer
— same role turbulence played, renamed), Lightning intensity. Every derived
value below defaults from the M/C/T cube and can be individually overridden.

**Precipitation type** (what's actually falling — separate from ground
state, see below):

```
if Temp < -1°C:
    Blizzard if Convection > 0.7 else Snow
elif -1°C <= Temp <= 0°C:
    Freezing Rain if Convection > 0.5 else Sleet
elif 0°C < Temp <= 1.5°C:
    Slushy Sleet / Wet Rain mix
else:
    Hail if Convection > 0.8 else Rain
```

Chosen specifically because it's a pure function of the instantaneous
Temp/Convection state — no transitionary/temporal type (e.g. "this drop
*used to be* freezing rain and is becoming sleet") needs to be tracked frame
to frame.

- Drop size varies continuously (drizzle → downpour) directly per-drop; the
  underlying drop *texture* is only regenerated when the size category has
  changed **and** hasn't been regenerated recently, to avoid texture churn
  from a slowly drifting size value.

**Ground state** (separate system — how precipitation behaves once landed):

| Band | Behaviour |
|---|---|
| Deep Freeze (< −5°C) | Puddles freeze solid; fast, dry powder snow accumulation |
| Wet Snow (−2°C to 0°C) | Packing snow |
| Slush & Sleet (0°C to 1.5°C) | Semi-transparent, reflective melt patches; snow accumulation capped and actively decaying |
| Cold Rain / Thaw (1.5°C to 5°C) | Puddles form; existing snow rapidly thaws |
| Warm (> 5°C) | Normal wetness shading; puddles evaporate as moisture drops to 0 |

**Convection thresholds** (cloud shape, fall behaviour, lightning cadence —
also drives the Clouds tab's volumetric field, below):

| Band | Cloud shape | Precipitation motion | Lightning |
|---|---|---|---|
| Stable (0.0–0.2) | Flat, uniform grey overcast, slow pan | Straight vertical descent | None |
| Breezy (0.3–0.5) | Slight definition, rolling shapes | Angled fall from wind force | None |
| Turbulent (0.6–0.7) | Dark, heavy, boiling cumulus | Choppy, stretched, faster fall | Every 30–60s, random |
| Severe (0.8–1.0) | Pitch-black, rapid pan, max churn | Chaotic/sideways, splash ripples on collision | Every 2–5s |

- **Forecast text** is generated from this same threshold data (e.g.
  "Thundery showers and a gentle breeze").

## Clouds tab (Volumetric Field + Sky Dome sub-tabs)

The tab is a thin container holding two sub-tabs, one per cloud layer —
the two layers are different systems with different authoring surfaces,
and keeping them on separate sub-tabs is what stops "cloud coverage"
meaning two things on one page.

- The **Sky Dome** sub-tab is the legacy Windlight/EEP flat-plane
  scrolling-noise layer's authoring surface. The layer itself is **kept,
  demoted to cirrus only** — it already looks right that high up and needs
  no rework — and this sub-tab exposes its full EEP parameter set
  (`SSAtmoEnvCloudDome`: colour, coverage, scale, variance, scroll rate,
  the density and detail triples, and the noise texture), every field
  keyframable and applied to the live sky by the applier. Cloud colour
  lives here, not with the Atmosphere & Lighting colours: it tints this
  layer and nothing else. Note EEP's "Cloud Coverage" slider genuinely
  drives `cloud_shadow`; the schema keeps the author-facing name.
- The **Volumetric Field** sub-tab configures the storm-capable layer:
  a small per-track queryable coverage/density/base-height/thickness field
  (`SSCloudField`, sketched in an earlier working session), driven by
  Convection (shape/churn/height) and Moisture (thickness/opacity).
- That field is the shared data source both rain-shaft placement and
  lightning-fork generation query — rain only spawns under columns above a
  density threshold, and lightning branches only grow through cells above
  that same threshold before exiting toward a strike point. This is what
  gives "rain that falls from the actual cloud shape" and "lightning that
  forks within the cloud" without a full volumetric raymarcher.
- The Volumetric Field sub-tab is the least settled part of the spec —
  treat its exact parameters as provisional pending an actual
  build-and-look pass. The Sky Dome sub-tab has no such caveat: its
  parameter set is EEP's own, transcribed.

## Planetary tab & System Designer

Authoring lives in a dedicated **System Designer** sub-floater
(`SSFloaterAtmoPlanetary`, opened per-track from the Planetary tab, which
itself keeps only the two distance-scale dials): an angled top-down orbital
model - sun(s) at the centre, orbit rings around them - with bodies selected
by clicking them (or their ring as a thin fallback), dragged along their
ring to set orbital phase, and edited in a scene-graph panel on the left.
Mousewheel zoom (0.5x-4x; a corner button cluster with reset-to-fit
between the plus and minus buttons); ring
spacing blends rank order with log-scaled authored radii so relative
distances read without AU-scale ratios flattening the inner system.

- **Hierarchy:** exactly three levels, Sun -> Planet -> Moon, no deeper
  nesting. Suns are capped at 4; planets and moons are uncapped.
- **Topology is automatic, not authored.** Parent/bound-partner selection
  was removed from the UI: the 1st sun is the root; the 2nd binds to it as
  a pair; the 3rd orbits that pair's barycenter; the 4th pairs with the
  3rd (two pairs, outer orbiting inner). `normalizeSunTopology()` re-derives
  this after every add/remove, so an isolated star (no orbit and no pair)
  is impossible. Planets always orbit the sun system's barycenter; moons
  orbit a planet (Add Moon targets the selected planet). A "giant primary
  with a dwarf companion" is just a pair whose mass ratio puts the
  barycenter inside the primary - no special case. The underlying
  `mParentIndex`/`mBoundPartnerIndex` schema is unchanged; a hand-written
  notecard can still express exotic layouts, the UI just no longer offers
  them.
- **Per-body fields:** diameter, mass, orbital radius, inclination, phase
  (position along orbit), axial tilt. Orbital/rotation periods are stored
  in schema but not surfaced (dead controls until motion exists).
  Eccentricity deferred. **Units are human:** orbital radius in AU for
  planets and km for moons; diameter in km (planets/moons) or solar
  diameters D (suns); mass in M - solar masses for suns, Earth masses
  below, sound because only same-level ratios ever feed a barycenter.
  Storage stays SI metres / relative mass.
- **Star type presets:** a sun-only Type dropdown (M/K Dwarf, G (Sol), F,
  A, B/O Giant, White Dwarf, Red Giant) setting diameter+mass; reads
  "(Custom)" when the values match no preset.
- **Automatic naming, SpaceEngine-style:** bodies not renamed by hand
  (tracked per-body via `mNameCustom`) auto-name from the first sun's name
  as the stem: "Sol", then "Sol B/C/D" for further suns, "Sol I/II/..."
  for planets in radius order, "Sol I.1" for moons - a renamed planet's
  moons follow its name ("Tatooine.1"). Recomputed on add/remove/radius
  change. New bodies place one step beyond the outermost sibling (+1 AU
  per planet, +1 Luna distance per moon).
- **Position model:** everything is fixed/authored, not simulated - radius +
  inclination + phase deterministically produce a fixed apparent direction
  and angular size (via distance falloff) for every non-home body. Stored
  periods exist so a future "actually animate this" toggle is a pure
  function-of-time evaluation rather than a schema change.
- **Binary pairs, mechanically:** a symmetric `mBoundPartnerIndex` per
  paired body; a third body orbits the pair by pointing `mParentIndex` at
  either member - the resolver substitutes their mass-weighted barycenter
  as the anchor. True N-body (3+ mutually orbiting) stays out of scope:
  no closed form, which would break fixed-now/simulate-later.
- **Home body:** exactly one, flagged "home"; supplies the axial tilt
  driving the sky's diurnal rotation - the one rotation every authored
  direction (emitters and billboards alike) is swept through.
  Shown in the designer with a house icon over the body, not a name suffix.
  A home body cannot also be a light emitter. **Default new asset:** an
  Earth-sized "Sol I" as home at 1.00 AU, orbiting a 1 M sun "Sol".
- **Light emitters:** hard-capped at 2 (the renderer's sun+moon light
  slots); UI disables further checkboxes at the cap. Creation-time
  defaults fill the second slot when free: a 2nd sun takes it, else a moon
  added to the home planet does (sun by day, moon by night) - defaults
  only, never re-asserted, never stolen. A notecard specifying more than
  two lights the first two in list order; nothing is deleted unless
  re-saved.
- **Distance scale:** two independent dials, Sun-Planet and Planet-Moon,
  compress authored orbital *distances* for artistic effect without
  touching any body's own physical size. These stay on the Planetary tab.
- **Rendering:** quad/billboard only for v1, with an equirectangular custom
  texture supported per body. True spherical geometry, real 3D rings, and
  any visible effect of axial tilt/spin are deferred until that rendering
  mode exists.
- **Day length:** stored per track, defaulting to the region's current EEP
  day-length/offset at creation time, free to diverge afterward.

## Rendering integration (the applier)

How the authored asset actually reaches the screen, phased:

- **Sink: EEP's ENV_LOCAL slot, deliberately.** The original Rendering
  model section said "does not plug into its personal-override slot"; that
  ambition meant a fully parallel sky/water renderer, which is months of
  shader work for a visual result EEP's pipeline already produces from the
  same parameters. Instead, `SSAtmoEnvApplier` (singleton) builds one
  `LLSettingsSky` + `LLSettingsWater` pair and mutates them per frame from
  the resolved Atmo Magic state, installed via
  `LLEnvironment::setEnvironment(ENV_LOCAL, ...)` - the same mechanism the
  Personal Lighting floater uses. The deviation is contained: the applier
  is the only class that knows the sink, so a future parallel renderer is
  a swap of that one class, not a rework of the resolvers or editors.
  EEP-proper source files remain untouched - injection is via public
  LLEnvironment API only.
- **Lifecycle:** active while the master switch is on and an asset is
  loaded; takes over ENV_LOCAL (stomping a Personal Lighting session is
  accepted and documented), clears it and restores the previous selection
  on deactivate. Per-frame it resolves the active track at the camera's
  altitude (same SSAtmoEnvTrackResolver call the precipitation bridge
  makes), evaluates every keyframed field at the current phase - the
  floater's preview override included, so scrubbing the editor scrubs the
  actual sky - and writes changed values through the settings setters.
- **Parameter mapping:** all Atmosphere & Lighting fields 1:1 (glow
  converted from UI-space to the packed glow colour at this boundary and
  nowhere else); all Water appearance fields 1:1 (three wavelet scalars
  fold into the LLVector3 setter). Water HEIGHT (the tide) is not applied
  to the render yet: the plane's height is region state, not a settings
  field, and overriding it client-side fights the sim - the tide currently
  feeds only the precipitation/runoff systems. Revisit deliberately.
- **Planetary lighting:** every celestial direction is authored position
  x diurnal rotation - each emitter's resolved home-relative direction
  from `resolveSky()` is swept by `resolveDiurnalDirection` (home body's
  axial tilt + the track's day phase), sun and moon slots treated
  identically, so orbital radius/phase/inclination all visibly place the
  lights and the designer canvas maps directly onto the sky. Slot ROLES
  go by apparent size, not list order: the emitter subtending the larger
  angular diameter takes the sun slot, the other the moon slot (ties to
  the lower body index). Emitter angular size maps to the sun/moon disc
  scale against the slot's QUAD angle (ss_atmoenv_quad_deg: a scale-1.0
  quad subtends 5.72 deg of sun, 5.15 of moon - the angle the geometry
  chain actually draws, which the old 0.53 deg reference described as ten
  times smaller, and every body drew oversized as a result); a body's
  optional disc padding (transparent art margin, 0 = full-bleed) inflates
  the quad by 1/(1 - 2*padding) so the VISIBLE disc lands on the authored
  angle. A non-null custom texture replaces the stock disc, and a null
  one falls
  back by the BODY's kind (sun-kind gets a sun disc, anything else the
  moon disc, whichever slot it landed in). How much the discs LOOM is the
  track's two distance dials themselves (see Distance scale) - they are
  the perception control: the human visual system reads the real Sun and
  Moon at roughly two to three times their true size, so the seeding
  default compresses both dials to 1/3 and the Space tab's Disc
  Perception radios (1x physical truth / 3x perceived / 8x cinematic)
  preset both dials to 1/N, while moving either dial by hand is the
  custom path and deselects the radios. An emitter coincident with
  the home body (distance ~0) has no sky direction and counts as absent.
  One placeable emitter parks the moon below the horizon (no phantom
  moonlight); none = a dim, sun-below-horizon sky rather than a fallback
  sun.
- **Billboard bodies:** every non-emitter, non-home body is published by
  the applier as an `SSAtmoEnvBillboard` (diurnally-swept direction,
  angular diameter, custom texture, resolved disc fraction) and drawn by
  `LLDrawPoolWLSky::renderHeavenlyBodies()` as a camera-facing quad after
  the sun and moon, through the moon shader - same disc-scale mapping
  (against the moon slot's quad angle, padding and all), sizing chain and
  horizon treatment as the moon, so equal angular diameters render equal, and positioned on the same effective shell the
  sun/moon quads occupy (their upstream camera-offset included), so
  nothing parallaxes apart as the camera moves. Null texture falls back
  by the body's kind - sun-kind bodies to the stock sun disc, the rest to
  the stock moon disc; bodies under 0.05 deg are skipped (subpixel
  shimmer). The whole pass
  gates on the applier's published vector being non-empty - one check
  when Atmo Magic is off.
- **Deferred, in order of intent:** cross-track sky/water blending on a
  soft crossing (precipitation already fades; sky currently cuts); the
  volumetric cloud field's noise/shaft/lightning rendering; water-height
  tides on the actual plane.

### Celestial quads and the sky shell

Every celestial quad - EEP's own sun and moon faces and our billboards
alike - sits on one camera-centred shell at HEAVENLY_BODY_DIST.

Upstream, `LLVOSky::updateHeavenlyBodyGeometry` bakes `mCameraPosAgent`
into the sun and moon face vertices and `renderHeavenlyBodies` then
translates by the camera origin as well, so the shell sat a whole camera
position vector away from the camera. At ground level that passes for
correct; in a 3000m skybox it threw the moon out to within a few hundred
metres of the cloud dome (radius 15000, scaled by 0.333 in `renderDome`,
so ~5000m out) and the two z-fought. The draw pass now subtracts the baked
offset from its own matrix, which fixes the sun, the moon and the
billboards together and leaves LLVOSky's vertex data - shared with the
reflection and glow paths, which want it in agent space - alone.

Two sky-dome terms were tuned against EEP's stock ~5.7 degree quad and
had to follow the discs when the honest quad angles shrank them: the sun
glow's forward-scatter hotspot (skyV) compresses its angular term by the
drawn disc's half-angle relative to the stock quad's (ss_sun_radius /
0.05, squared - the term grows as the square of the angle), so the glow
hugs whatever disc is drawn; and the weather corona (skyF ssOptics) scales
every one of its angles by the same ratio, keeping it a rim around the
disc. The crystal halos stay at true angles - droplet and ice optics are
the weather's, not the disc's. Both fall back to stock's fixed angles
while no Atmo environment drives the sky.

### The default world

A new environment is Earth: 23.44 degrees of axial tilt, 7.155 degrees of
orbital inclination (Earth's own orbit *is* the ecliptic and would read as
zero, so the number worth carrying is its inclination to the Sun's
equator - the plane bodies here orbit in), and orbital phase 0.

**Orbital phase is a season dial here, not a heliocentric longitude.** The
reference plane contains the rotation axis at zero tilt (see
`celestialAxis`), so a home body's phase is really the angle between its
sun and the celestial equator. Phase 0 puts the sun on the equator: rising
due east, culminating at 90 - tilt = 66.6 degrees, which is an equinox at a
temperate latitude and the most useful place to start authoring from.
Transcribing Earth's J2000 mean longitude of 100.46 degrees into this
field instead puts the sun 19 degrees from the celestial pole, which in
this convention is polar night - a default world whose sun never rises at
all. (Verified numerically, not assumed: that configuration peaks at -4.16
degrees of elevation.) An author wanting a season dials phase a few tens of
degrees either way; a quarter turn is the arctic.

It comes with Luna - real diameter, real distance, 5.145 degrees of
inclination - as a light emitter, at the planet's own orbital phase, which
puts it opposite the sun: a full moon, up all night. (Half a turn from
there would be a new moon, i.e. a default world whose moon is never visibly
anywhere.) Slot assignment gives Sol the sun and Luna the moon by physical
diameter, so both lights an author expects are there from the first frame.

### Seeding the day cycle from a sky set

Creation fetches a configured set of PBR sky assets (`SEED_SKY_ID` in
ssatmoenvmanager.cpp - currently six authored skies; the four stock
non-legacy ones are archived in the comment beside it) and keyframes each
where the track's own sun actually puts it.

Placement is **measured, not assumed**. A sky carries the sun position it
was painted for, so each one lands at the phase where this world's sun
stands closest to standing there - `phaseForSunDirection`, which maximises
the dot product between our swept sun direction and the sky's authored one.
That is a closed form, not a search: the dot product is the same
constant-plus-sinusoid shape the elevation curve has, so its peak is one
`atan2`.

Matching the **whole direction** rather than just its height is what makes
this work for an arbitrary set of skies. Every elevation below the peak
happens twice a day, so elevation alone cannot tell a dawn sky from a dusk
one - but their suns sit on opposite sides of the sky, which a direction
match reads immediately. Nothing in the code knows how many skies there
are or what times of day they depict, so changing the set is changing the
list and nothing else. An unreachable sun position (a sky painted for a
higher sun than this world's ever climbs) still resolves cleanly to its
closest approach.

If two skies measure to the same instant - most likely a set authored as
looks, with the sun left wherever it happened to be - the whole set falls
back to an even spread in list order, with a warning. All-or-nothing
rather than nudging the colliding pair: either every sky sits where its own
sun puts it or none does, and a half-measured cycle would be impossible to
reason about from outside.

This matters more than it sounds: with the default 1 AU orbit the sun
culminates at phase 0.75, so any fixed-phase placement would put every sky
a quarter-cycle from the sun it describes. A collapse-if-constant pass then
drops fields the whole set agrees on back to plain values, so a notecard
does not carry six identical keyframes for every dial that never changes.

Dropping a full-perm EEP sky settings item onto the floater stamps its
whole look as keyframes at the current preview head - scrub to dusk, drop
a favourite sunset, and that instant now looks like it.

### Rise and set markers

The preview scrubber carries always-on markers for where the selected
track's lights cross the horizon: a full-size warm amber up/down triangle
pair for the sun, small moonlit blue-white ones for the moon. Size does
the legending - the bigger light gets the bigger glyph. They come from
`SSAtmoEnvPlanetaryResolver::riseSetPhases`, which solves the same
elevation sinusoid the diurnal rotation produces, and from
`resolveLightRoles`, which is also what the renderer asks - so a marker
can never annotate a different body than the one that actually rises. A
body that never crosses the horizon draws nothing rather than parking a
marker at an edge: on a world where the sun never sets there is no sunrise
to point at.

## Weather-driven sky modulation

Built. The weather cube drives the EEP-rendered sky through a single pure
function, tuned per track from the Weather Influence sub-floater.

- **Modulation, never mutation.** Authored keyframed values are evaluated
  exactly as written and then bent on their way to the setters, every
  frame, from the same SSAtmoEnvWeatherState that drives precipitation.
  Authored data is never touched - the same philosophy as the
  gust/lightning Auto split - so turning a mapping off renders precisely
  the authored sky.
- **Shape:** `SSAtmoEnvSkyWeatherModulator::compute(input, influence)`
  returns an `SSAtmoEnvSkyModulation` whose methods are applied at each
  setter (`mod.hazeDensity(atm.mHazeDensity.valueAt(phase))`). A
  default-constructed modulation is the identity, so the applier calls
  them unconditionally instead of scattering "is this on" branches. All
  formulas and their full-effect constants live in
  ssatmoenvskymodulator.cpp and nowhere else; a strength is always "how
  much of this mapping", never a raw parameter value.
- **Mappings** (each an independent enable + 0..1 strength):

  | Weather input | EEP parameters | Shape |
  | --- | --- | --- |
  | Deck coverage | dome overcast band coverage (cloud_shadow) | the band tracks the deck's live coverage - lift only, authored coverage is a floor; one coverage, two layers |
  | Wind heading/speed | cloud layer drift | the deck actually travels over the world - see below |
  | Convection | dome scroll rate (churn) | scroll is evolution, not travel - see below |
  | Precipitation intensity | water fog modifier | multiplier, so rain thickens whatever the author set; the old moisture -> haze mapping (haze density up, distance multiplier down) is retired - on skies authored with heavy haze the lifted airlight blew whole scenes out, and the authored haze now renders exactly as keyframed |
  | Convection (past 0.55) | gamma and ambient down | the design's "pitch-black boiling sky": dimmer light, heavier deck - the deck's own gloom and thickness carry the weight, and the overcast band tracks the deck rather than being pushed past it |
  | Convection (anvil, 0.6-0.9) | cirrus veil altitude | the veil descends from its authored height onto the deck's lid, ending ~300 m over the deck's max height - the deck integrates with its cirrus |
  | Sub-freezing AND dry | sky ice level up, blue density crisper | both gates required - freezing fog is not a halo sky |
  | Rain just stopped, sun up and low | sky moisture level (EEP's rainbow driver) | decays over 4 minutes; needs sun below ~42 deg |

- **Scroll is churn; drift is travel.** EEP's cloud scroll rate is added to
  `cloud_pos_density1` and to nothing else (llsettingsvo.cpp), so it slides
  the large cloud texture against the small one that stays put: the pattern
  boils and reforms in place rather than the sky moving over the world.
  Convection drives it for that reason - wind does not churn a cloud, it
  carries it.

  Actual travel is its own thing: the modulator publishes a drift velocity
  in metres per second, the applier integrates it (on the wall clock, so the
  sky keeps moving while an editor's scrubber holds the phase still), and
  `cloudsV.glsl` shifts the whole layer's UV by the result before the other
  texcoords are derived from it - so every texture layer in the deck travels
  together. The metres-to-UV conversion is the one the region-parallax term
  already uses, `1 / (16 * max_y * cloud_scale)`, which means a higher deck
  drifts more slowly for the same wind, exactly as it should.

- **Cloud variance is not a churn dial — and no longer a storm dial at
  all.** EEP's variance displaces the cloud noise lookup and then applies
  `cloudDensity *= 1 - variance^2` (cloudsF.glsl), so raising it *erodes*
  the layer. An early version of the storm mapping drove it hard for
  "boiling cumulus", which made turning convection to maximum visibly pull
  the clouds back - the opposite of a storm sky. It was then retuned to a
  small bump for texture, with the weight of a storm coming from coverage
  instead - but the bump survived the volumetric split that demoted this
  layer to cirrus duty, and on the demoted layer the erosion has nothing to
  eat into but the overcast sheet max moisture builds: past roughly
  convection 0.8 the shader's density cut saturates across the sheet and
  tears it open - gaps, with the disturbed lookup peeling at their edges.
  The bump is now gone entirely: convection never touches the dome. The
  storm's texture lives in the deck's churn and flow, its weight in
  coverage, exactly as the coverage-first retune intended.
- **The one piece of state:** rainbows happen *after* rain, which no
  evaluation of "right now" can know, so the applier keeps a rain-stop
  trail (`mSecondsSinceRainStopped`, wall-clock so it decays the same at
  20fps and 200) and passes it in. The modulator itself stays pure.
- **Control surface:** `SSAtmoEnvWeatherInfluence`, per track, serialized
  as `weather_influence`, deliberately NOT keyframable - it states how a
  world behaves, not what it looks like at 3pm, and keyframing it would
  mean authoring the same storm twice. Defaults are master-on with every
  mapping at full strength: a weather system whose weather does nothing to
  the sky is the surprising configuration, and a notecard written before
  the struct existed loads as that same default rather than as a third
  behaviour.
- **Editor:** one Weather Influence floater (`ss_atmo_influence`), opened
  from a button on each tab a mapping reaches - Weather, Water, Clouds >
  Sky Dome, Atmosphere > Sky. One floater rather than four sections
  because the mappings have to be compared against each other to be tuned
  ("is my storm darkening fighting my rainbow window?"). Each row shows what that
  mapping is doing right now, read from the applier's last modulation, so
  the readout is what the renderer actually did rather than a second
  opinion.

## Surface water: what was retracted, and what replaced it

The drainage simulation is gone. What it was: a per-region trace that solved
a flow network over ~16k cells - flow accumulation, flood fill, catchment
per cell, eaves sorted by how much roof drained through them - and then shed
drips and streams according to what arrived at each lip. It was a great deal
of machinery for a look that did not depend on most of it, and its routed
answers were unstable: which way a nearly-level cell drained flipped between
retraces on capture noise, moving puddles around and re-pointing the streak
direction under a surface that had never moved.

**Nothing carries water sideways any more.** What replaced it is local and
static, per cell of the same captured surface grid:

- **Slope.** The height field's own gradient - which way this cell runs and
  how steeply. That is the whole directionality model: a pitched roof
  streaks downhill because it is pitched and being rained on, not because a
  solver routed a catchment through it. It feeds the shading window's second
  channel, where it replaced the routed flow direction. (That channel can
  now be filtered linearly too: a gradient field has no head-on
  discontinuities to blur, which is what forced nearest filtering before.)
- **Dips.** A cell flat enough to hold water that no neighbour drains -
  asked of its eight neighbours and nothing further. Puddles form there,
  fed by the rain landing in them and capped, rather than sharing out a
  catchment. The cap is the point: an unbounded catchment share let one
  hollow stand absurdly deep while the cell beside it held a film.
- **Lips.** A cell with open air, or a long drop, beside it. This is where
  drips and streams come off, and it is found by looking at neighbours -
  there is no network and no eave list.

**Streams, drips and eave curtains are kept.** They were never the problem;
what fed them was. Each lip cell now holds its own reservoir: rain fills it
at a rate set by the cell's own footprint plus an allowance inferred from
the pitch behind it, and it drains on a time constant. That reservoir is
what makes an eave keep running for a while after the rain stops, and what
keeps it running *through* a gust lull instead of stuttering in time with
the weather - which is the behaviour the old `mStore` existed for and the
one part of it worth keeping. Past a threshold rate a lip stops shedding
drops and puts up a curtain, exactly as before.

## Avatar wetness

Avatars soak on their own clock, in `ssavatarwet.*`.

They used to pick up the ground's wetness by accident: the surface field is
indexed by XY, so a fragment on someone's shoulder read the wet pavement
under their feet and shaded like it. `ssFieldAt` now declines to answer for
any fragment standing above the surface it stored, which fixes that at the
source - and the effect it accidentally revealed is now built deliberately.

- **Exposure** comes from the same captured surface: if the stored surface
  above someone is over their head, it is sheltering them. Fades across a
  band rather than switching, so walking under an awning reads as an edge
  instead of a line.
- **Accumulation** is an exponential approach with the rate carrying the
  intensity, not the target: a drizzle will eventually soak someone who
  stands in it all day, just far more slowly than a downpour would. Scaling
  the target instead would cap them permanently damp.
- **The gradient** runs up the body. Each height band has a soak threshold
  it starts wetting at, taken from whichever of two curves reaches it first
  - distance from the head, and distance from the feet - so head and
  shoulders darken first, feet follow close behind (splashback, not rain),
  and the middle of the body fills in last and only really in a downpour.
- **Decay** is much slower than soaking, for the same reason puddles drain
  slowly: a soaked coat does not stop being wet when the cloud passes.
- Shading is a capsule test per avatar in the existing wet pass, capped at
  the nearest eight - a crowd is a wall of bodies, and each one costs the
  whole screen another test per fragment. No puddle term: water stands on a
  floor, not on a person.

## Volumetric cloud field

The cirrus dome EEP draws is a texture on the inside of the sky: no
altitude, no thickness, no position in the world. You cannot fly through
it, it never sits below a mountain top, and a storm cannot tower over you
in it. `SSAtmoEnvCloudField` has authored a base height, a thickness, a
coverage and a churn since phase 7; `ssvolcloud.*` is what finally puts
them on screen.

It is a **puff field, not a raymarched volume**: a deterministic scatter of
camera-facing textured quads at the field's own altitude, sized by its
thickness and thinned by its coverage. That is a real simplification and
worth naming - a raymarch would light correctly from inside, and this
cannot - but it draws in one pass on any hardware the viewer already runs
on, it sits at true world altitudes, and it travels with the wind.

- **Cells belong to the air, not the ground.** Cell coordinates are taken
  in the wind's frame (camera position minus accumulated drift), so a puff
  keeps its identity while the field slides past and new ones arrive from
  upwind. On a ground-fixed lattice the puffs would stand still while the
  dome behind them moved.
- **Coverage decides which cells hold cloud**, by comparing a stable
  per-cell hash against it. Because the hash is stable, the cells that
  survive at low coverage are the same ones that were there at high
  coverage - the field thins rather than being re-scattered every time the
  weather eases.
- **Art follows the weather**: a towering, churning field wears the
  cumulonimbus map, a quieter one altocumulus. The layered (cirrus-like)
  map stays on the dome, where a flat high deck belongs, and is what a new
  environment starts on.
- **Depth tested, not depth written**: the layer sits behind a mountain
  that stands in front of it, and its own quads blend through each other
  instead of the nearest punching a hole in the rest. Sorted back to front,
  capped per frame, and the far end of the list is what gets dropped -
  those are the small ones on screen.

Still missing from the layer: shafts, lightning forks, and any lighting
that changes as you fly into it.

## How celestial bodies are lit

A body's disc is shaded as the **sphere it stands in for**, not as a flat
lamp. The billboard already carries the normal implicitly - the disc is the
projection of a sphere, so the UV gives it - and the fragment shader
rebuilds that sphere from the quad's own axes, takes N.L against the
direction of the body's star, and softens the terminator (a hard cut on a
small disc reads as a bite taken out of it). A moon a quarter of the way
round its orbit is therefore drawn as a quarter moon with nobody authoring
a phase.

- **The star that lights things is the largest SUN-kind body**, not
  whatever holds EEP's sun slot. That slot is a rendering role and could be
  a moon on a world with no star; what lights a moon is a star.
- **Both halves are authored per body, not derived.** `mEmissive` draws the
  disc at full brightness with no dark side; `mPhaseShaded` shades it as a
  sphere. Each defaults from the body's kind when it is created - a sun
  lights itself, everything else is lit by one - and is editable from there,
  because a glowing artificial moon, a lava world or a magical second sun
  that is technically a planet are all things a world might want, and
  deriving the behaviour from kind would make them unauthorable. Emissive
  takes precedence: something making its own light has no dark side to draw
  across it, so the phase control greys out rather than quietly doing
  nothing.
- **Eclipses** darken a body inside the home world's shadow, modelled as a
  cylinder with a soft edge rather than a proper umbra/penumbra cone - at
  these distances the difference is the softness, which is put back by
  hand.
- **The unlit side is not black** (`SS_PHASE_EARTHSHINE`): a new moon is a
  disc lit by the light its own planet throws back at it.
- EEP's own moon quad gets the same treatment through uniforms added to
  `moonF.glsl`, guarded by `ss_phase_enable` so stock rendering is
  untouched. Its brightness is additionally scaled by the phase as a scalar,
  which is what makes a new moon light the world less than a full one.

`SSAtmoEnvCelestialBody::mBrightness` remains as an authored multiplier on
top of that - per body, because a sky has exactly one Moon Brightness and a
world can have several moons. Masser and Secunda are not equally bright,
and the billboard path used to bind the sky's single value for every body
it drew, so they all lit and dimmed together.

## Keyframes

- Universal per parameter (slider, colour, dropdown/enum) — no
  whole-sky-snapshot keyframe concept like EEP.
- **Rule:** no keyframe on a param → plain permanent value. Head sitting on
  an existing keyframe → editing the value edits that keyframe. Head *not*
  on an existing keyframe → editing the value inserts a new one there.
  Non-tweenable values (e.g. a forced precipitation-type override) hold from
  one keyframe up to, but not including, the next.
- **Curves**, hidden behind a simple interface: Ease-in-out by default,
  Linear where more appropriate, Hold/step for non-tweenables. No
  curve-editing UI exposed in v1 — defaults are chosen per field type.
- **Editing surface:** an After-Effects-style 2D parameter table — one row
  per parameter: label, slider, numeric input, a keyframe-diamond toggle, and
  chevrons either side of it to jump the preview head to that parameter's
  previous/next keyframe (wrapping around the loop). A dot/tick strip under
  each row mirrors the shared timeline position against that parameter's own
  keyframes. The master scrubber at the top of the same column sets preview
  time only — it is not itself interactive for keyframes.

## Deferred / explicitly out of scope for v1

- True N-body simulation beyond hierarchical binary pairs.
- Live/simulated orbital or rotational motion — schema is ready, math isn't
  wired up.
- Spherical-geometry celestial bodies and real 3D rings.
- Orbital eccentricity.
- Seeder-feeder cross-track rain interaction (out, per the multiple-isolated-
  tracks discussion).
- Multiple distinct water bodies within one track.
- A full drag-and-tween graph editor for keyframes (v1 ships the simpler
  table + popup model).

## Open items needing one more decision

- **Existing legacy notecards, on upgrade.** Since Atmo Magic retires the
  legacy weather layer's format rather than running alongside it forever,
  there's no live tag collision to disambiguate — but any `atmo:<uuid>`
  notecard already saved in the *old* format is still sitting on parcels out
  there. Does an Atmo Magic viewer encountering one of those (i) silently
  ignore it and fall back to no-weather, since it can't be told apart from a
  corrupt/foreign document without inspecting it, (ii) auto-migrate it — read
  the old `precipitation`/`turbulence`/`wind_*` keys and derive an equivalent
  Moisture/Convection/Temperature-cube starting point — or (iii) is a clean
  break acceptable, since this is a personal client-side feature and not
  something anyone has a durable dependency on? Not decided yet.

## Implementation status

Everything below is written and build-verified (compiles and links clean
against `firestorm-bin`); "wired" means it's actually consumed by something
that runs, not just sitting next to it.

| Phase | Files | State |
|---|---|---|
| 1 — schema, notecard round-trip | `ssatmoenvasset.*`, `ssatmoenvmanager.*`, `ssfloateratmoenv.*`, `floater_ss_atmo_env.xml` | Done. Inventory `New Atmo Magic` creation wired into `llviewerinventory.cpp`. |
| 3 — keyframe engine | `ssatmoenvkeyframe.h` | Done. Proven end-to-end on `SSAtmoEnvWeather::mMoisture` in the floater. |
| 4 — multi-track resolution | `ssatmoenvtrackstate.*` | Done. Not yet fed a live camera position — pure function, no agent hookup. |
| 5 — weather derivation | `ssatmoenvweatherstate.*` | Done. Forecast text wired into the floater as a proof. |
| 6 — planetary | `ssatmoenvplanetarystate.*` | Done. Billboard rendering wired: non-emitter bodies drawn as quads in `lldrawpoolwlsky.cpp` via the applier's published list. |
| 7 — cloud field | `ssatmoenvcloudfieldstate.*` | Done. Derivation only — no noise field, no shader. |
| bridge — Atmo Magic → shared renderer translator | `ssatmoenvbridge.*`, spliced into `ssatmomagic.cpp`'s `refreshParams()` | **Verified live.** Loading an Atmo Magic environment and raising Moisture produces actual visible rain through the shared `SSAtmoMagic` renderer - confirmed in a running client, not just compiled. |
| 8 — parcel/Bridge discovery | `ssatmoenvdiscovery.*`, calling `FSLSLBridge::viewerToLSL` (HTTP, not chat), bootstrapped from the existing per-frame Atmo Magic touch-point in `llviewerdisplay.cpp`; LSL side is the `FetchNotecard` command in `indra/newview/fs_resources/EBEDD1D2-...-D47BBCA5DFB.lsltxt` | **Verified live end-to-end.** A parcel's `atmo:<uuid>` marker triggers the Bridge fetch, the named environment loads, and it actually drives the renderer (see the bridge row above) - confirmed in a running client. |

The floater's editing UI is now complete across all six tabs (Track,
Atmosphere & Lighting, Clouds, Planetary, Water, Weather): per-track
selection on a vertical altitude rail, phase-based per-parameter keyframes
with a shared preview scrubber, keyframe ghost overlays (per-row with value
labels, tab-wide from the scrubber, value-position ghosts on each slider),
the Clouds tab split into Volumetric Field and Sky Dome sub-tabs, the
Atmosphere tab split into Atmosphere and Sun & Moon sub-tabs (mirroring
EEP's own Fixed Environment tab structure), and the Atmosphere & Lighting
schema fully typed at full EEP parity (the PBR-era sky-moisture/droplet
radius/ice/probe-ambiance dials and moon brightness included; sun/moon
position deliberately absent, computed by Planetary's home body instead).

The rendering integration is live end to end: SSAtmoEnvApplier drives
EEP's ENV_LOCAL sky/water from the resolved asset every frame (all
Atmosphere & Lighting fields, all Sky Dome cloud-layer fields, all Water
appearance fields, with per-field
change detection), the light emitters drive the actual sun and moon
(authored sky positions swept by the diurnal rotation about the home
body's tilted celestial axis, the physically larger emitter taking the sun
slot - a nearby moon out-subtends a distant giant, so apparent size cannot
decide this; dark world with no emitters), and non-emitter celestial bodies
render as textured billboards
in the sky pass, depth-correct against the stars and sized pixel-equal
with the moon at equal angular diameter, every celestial quad on one
camera-centred shell. The floater's preview scrubber scrubs the real sky.

Weather now reaches the sky as well as the particle system:
`SSAtmoEnvSkyWeatherModulator` bends the authored values on their way to
the setters, per-track and per-mapping, tuned from the Weather Influence
floater (`ssfloateratmoinfluence.*`,
`floater_ss_atmo_weather_influence.xml`). A new environment seeds a full
day cycle from a configured set of PBR skies, each placed by matching its
own authored sun direction against this world's sun.

Not yet done anywhere: cloud noise field, rain shafts, lightning forks -
schema, editors and derivations exist, but the volumetric layer still only
reaches the screen through what SSAtmoEnvBridge translates into the legacy
particle/wind pipeline. Also still open: water-height tides on the actual
plane, cross-track sky blending on a soft crossing, and ring rendering.
