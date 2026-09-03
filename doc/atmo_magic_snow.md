# Atmo Magic: blowing snow, drift and whiteout

Snow already settles: `SSPrecipPreset` carries `mSnowRate`/`mSnowMelt`/`mSnowDepth`/`mSnowRepose`,
`SSSurfaceField` integrates it per region cell (`sssurfacefield.cpp`, `tick()`, slope-gated by
`lieHere()`), and the field window ships the settled depth to the shaders. What does not exist yet
is everything the wind does to that snow once it is down - and everything the wind-driven snow does
back. This doc is the design for that layer: pickup, drift, ground blizzard, squall whiteout, and
the snow surface treatment, all built on the machinery Atmo Magic already runs.

The physics ladder, and how it maps to what follows:

| Condition | Threshold (typical) | Effect |
|---|---|---|
| Calm | < 3 m/s at the surface | snow only accumulates |
| Saltation onset | 3-8 m/s, ramping | settled snow lifts off exposed ground and streams downwind |
| Ground blizzard | sustained strong wind over bare deep snow | continuous mass lift everywhere exposed; visibility collapses near the ground |
| Snow squall | heavy falling snow + gusty + cold | falling snow plus drift plus whiteout, locally near-zero visibility |

The whole design hangs off one reading: **the wind flowmap already knows where the wind is faster
than ambient** (`SSWindFlowMap::sample`, alley jets, rooftop lift, lee calm - capped by
`SSAtmoWindFlowMaxGain`, default 4x). Every threshold below is evaluated against that local field,
not the sim's uniform breeze, so drift starts in the alleys before it starts in the open, and a
narrow gap whites out before the plaza does.

## What exists and what this adds

| Subsystem | State | Role here |
|---|---|---|
| `SSWindFlowMap` (`sswindflow.h/cpp`) | live | the wind field itself: `sample()` returns the gust-modulated flow at a point, `exposure()` how sheltered it is, `surfaceAt()`/`forEachColumn()` walk solid ground |
| `SSRainShadowMap` | live | top-down surface capture; `resolveColumn()` is where every particle finds its floor |
| `SSSurfaceField` | live | per-region wet/snow/puddle fields; **snow is stored but not yet shaded** - no surface pass reads the snow channel today |
| `SSPrecipSim`/`SSPrecipRenderer` | live | deterministic particle sim with tier bands, cell-hash spawning, flowmap advection (`windAt`, `ssprecipitation.cpp:84`) |
| RISER archetype ("Mana Embers") | live | particles spawned **from the ground**, rising - the exact emission shape blowing snow needs |
| `SSAvatarWet` | live | per-avatar capsule soak, folded into the wet pass - the template for snow caking |
| Weather state (`SSAtmoEnvWeatherState`) | live | wind speed, gusts, temperature, intensity bands - the knobs whiteout and squall derive from |

The one genuinely new render pass is whiteout. Everything else extends existing passes and sims.

## 1. Pickup: erosion from the surface field

`SSSurfaceField::tick()` gains a wind branch, active when the preset is a snow preset
(`mSnowRate > 0`) and the preset opts in with a lift rate (below). Per cell, at the field's own
tick:

```cpp
// the wind the cell actually feels, ground level, gusts included
const LLVector3 flow = SSWindFlowMap::getInstance()->sample(cell_pos);
const F32 v = flow.magVec();                 // sample() already applies gustAt scale + veer
const F32 lift = smoothstep(lift_lo, lift_hi, v);   // 0 below the band, 1 above
```

Two things make this the right gate:

- **`sample()` already includes the gust envelope** (`gustAt` is applied inside `sample()`,
  weighted by the cell's `exposure`), so gust-driven pickup - the pulsing quality of real blowing
  snow, lines of it jumping off a roof edge as a gust arrives - is free. No separate gust pass.
- **Corridors jet, plazas do not.** With the default `SSAtmoWindFlowMaxGain` of 4, a gap between
  buildings runs up to 4x ambient, so with an ambient 3 m/s breeze only the alleys saltate; in a
  storm with 8 m/s ambient, everything does. The threshold is evaluated locally, which is what
  makes the *same* preset produce drift on the exposed shore and calm behind the wall - and makes
  the user-facing band land where the literature puts it: onset around 3-4 m/s, full transport by
  8.

Defaults ship as settings, because the threshold is a physical constant, not an art dial:
`SSAtmoSnowLiftLo` (3.5 m/s), `SSAtmoSnowLiftHi` (8 m/s). The opt-in and the rate are authored per
preset: `mSnowLiftRate` (0 = the type never blows; "Blizzard" gets a strong rate, "Snow" a light
one). A temperature gate rides on top - above ~1.5 C the preset's own `mSnowMelt` already fights
accumulation, and wet snow does not blow, so lift scales down toward zero as temperature climbs
past 0.

Erosion removes depth: `mSnow[i] -= min(mSnow[i], lift_rate * lift * dt)`, and the removed mass
becomes the drift tier's spawn budget (below). The `lieHere()` repose clamp still governs how much
snow a cell can hold, so an eroded slope cannot take more than it has room for when it re-deposits.

## 2. Drift: snow advected by the flowmap, emitted from the ground

The user-facing ask is "snow blowing across the wind flow map from the ground, like the mana
embers, but snow" - and the RISER archetype is already that shape: `risesFromGround()` spawns each
particle at `resolveColumn()`'s hit, gives it an upward `mFallSpeed`, and hands it the wind sampled
at the ground (`emitParticle`, `ssprecipitation.cpp:676`). Mana Embers rise; blowing snow *streams*
- same emission point, different velocity profile and a spawn gate the embers do not have.

**Recommended form: a drift flavour of the RISER archetype**, not a new tier. A new `SSPrecipTier`
would grow `TIER_SPEC`, every preset's tier array, the renderer's bucket matrix and the preset
serialisation for one behaviour that the riser path already covers; the differences we actually
need are parameterisable:

- **Spawn gate** (`spawnTierCell`, riser branch): a cell emits only if the surface field reports
  snow at that cell (`SSSurfaceField::sample()` - one grid lookup, already region-anchored) AND
  the local lift figure from section 1 is above zero. The spawn weight carries that lift figure,
  so density is proportional to how hard the wind is working the snow there, not to area.
- **Velocity**: horizontal dominant. `mFallSpeed` becomes the loft (a gentle climb, a few tenths
  of a m/s, decaying over the particle's life so flakes arc up, stream, and settle back); the
  horizontal velocity is `windAt(hit_pos)` - which is the flowmap, gust included - times
  `mWindResponse`. A blizzard preset with `mWindResponse` 2.5+ funnels through the same alley jets
  the banner prims lean in.
- **Look**: saltation is near-surface. The riser's short life (2-3 s) and the `VIS_BAND` floor gate
  already keep the tier hugging the ground; `PART_GUSTY` (preset `mSway >= 1.5`, which Blizzard
  already sets) adds the vertical tumble. `applyEmberFlavor`-style per-particle flavouring is
  where a snow variant can mix shard/streak sprites for the streaky, motion-blurred read AC
  Shadows gets by stretching particles along velocity: `KIND_STREAK` with size scaled by the
  particle's own speed is the cheap version, and the streak path already exists.
- **Determinism**: unchanged. Spawns come from the shared-clock cell hash; the flowmap underneath
  is itself a deterministic solve, so every viewer lifts snow from the same corners of the same
  alleys.

What this buys visually, for free, because the emission points at the ground: snow visibly
*originates* at the surface and moves with the flow around obstacles - the lee of a wall stays
quiet, a rooftop edge trails streamers, a courtyard stays clear while the street outside its gap
streams. That is the ground-blizzard read, and it is a mass effect: when the ambient wind holds
above the threshold and the field is deep, every exposed cell qualifies, the tier caps (the riser
budget shares) saturate, and the air near the ground fills. No new "blizzard" code path - it is the
steady state of the same gate.

Redeposit closes the loop: in `tick()`, cells with a low lift figure but a sheltered flow exposure
(the flowmap's own `exposure()` channel - the calm the lee already has) gain back depth at a
deposit rate, capped by `lieHere()` room as today. Snowfall + wind therefore scours the open
ground and banks drifts against windward-obstacle lees and inside courtyards, and the field - the
same texture the shaders and footstep queries already read - is the whole record of it. This is the
tie-in the accumulation system makes cheap: drift is just erosion and redeposition on the existing
wet/puddle field, with the flowmap as the transport term.

## 3. Whiteout

A local, screen-space fog pass - `SSWhiteoutF`, in the same deferred-post family as the wet pass -
rather than a global haze change, because the whole point of wind-driven visibility loss is that it
is *structured*: the corridor whites out, the courtyard next to it does not.

**Insertion point**: beside `doAtmospherics()` in the pool pass (pipeline.cpp:4453 block), after
the global atmospherics so it composites on top of them, before the weather render block
(pipeline.cpp:4461) so falling flakes and drift particles stay in front of their own fog - the
same reason rain draws after clouds. The known trade is the one already documented for weather
there: a window between the camera and the whiteout shows the whiteout behind the glass.

**Density model** - three factors, all already computed somewhere:

1. **Drift veil** (wind-driven): per pixel, reconstruct the world position from depth, and march
   the eye ray through the drift band - the slab from the surface up `SSAtmoSnowBand` (1-3 m).
   Surface height per cell comes from the surface field window the shaders already bind
   (`ssFieldFetch`); flow speed comes from a new small window over the flowmap's bottom slab
   (same camera-centred window pattern `SSSurfaceField::updateWindow()` uses). Local speed above
   the ambient (`atmo->wind().magVec()`) by more than the `SSAtmoWhiteoutCorridor` ratio is the
   corridor term - the narrow-alley jet the user called out - and it is exactly what makes the
   gap between two buildings light up white while the plaza beside it stays clear.
2. **Squall veil** (fall-driven): background density everywhere the pixel's column is open sky,
   proportional to falling intensity (`atmo->precipitation()` when the preset is a snow type).
   Heavy falling snow alone, with no wind, still costs you the far hillside; this is the term that
   pays for it. Open-sky test reuses the exposure march `ssSurfaceFieldF.glsl` already runs
   (the `w` channel of `ssFieldAt`) - under an eave, no squall veil.
3. **Ground-blizzard coupling**: the drift veil's intensity is the same lift figure section 1
   computes, integrated over the column - sustained strong wind over deep snow saturates it, which
   is the ground blizzard as a visibility state rather than a separate system.

Colour is the environment's own fog colour at that depth (the same one atmospherics uses), so the
whiteout tints with the weather rather than being a hardcoded white; as density saturates, scene
colour lerps to fog colour exactly - a true whiteout. Depth falloff is exp-style over
`SSAtmoWhiteoutRange` so the near field stays readable while everything past a few tens of metres
goes.

Interiors are safe by construction: both factors need the column to be exposed, and the
exposure march already answers "is anything above this fragment" - the same test that keeps rain
off the inside of porches keeps whiteout off the inside of rooms.

Sky pixels (no depth) take the squall veil only, faded by distance toward the horizon - the storm
deck and atmospherics carry most of that look already, so the sky term stays modest.

**The display-density layer.** The march above is static structure - it says where fog *can* be,
not where it *is right now*. One layer adds the time dimension: a per-region 2D airborne-density
texture (~128², `SSAtmoSnowDensityRes`), GPU ping-ponged every frame - advected by the ground
flow window and relaxed toward a CPU-authored target (lift x depth, packed into the ground
window's spare channel). The whiteout multiplies its drift/squall terms by this texture, so gust
fronts sweep as visible fog waves, and the air stays hazy for seconds after a gust passes. Two
facts make this cheap where a 3D volume was refused: the flowmap is divergence-free by
construction, so advection over it needs no solver of our own, and the *targets* - not the sim -
own the ground truth. This is the stack's one approximate layer, by rule: **ground state exact,
air approximate** - the texture is never read by any state-owning system, and nothing feeds back
from it into spawning, erosion or deposits (field -> targets -> air, one way). At High quality
the drift pool can splat enrich the targets (actual particle positions added onto the analytic
floor, never replacing it).

Interiors are safe by construction: both factors need the column to be exposed, and the
exposure march already answers "is anything above this fragment" - the same test that keeps rain
off the inside of porches keeps whiteout off the inside of rooms.

## 4. The snow surface shader

The accumulation field is done; the *shading* is the missing half. The wet pass pattern -
fullscreen pre-lighting pass reading the field window, scratch target, commit into the gbuffer
attachments (`sssurfacefield.cpp:919`, hooked at pipeline.cpp:9761 ahead of all lighting) - is the
correct vehicle, and the new snow pass slots into the same block:

- **`gSSSurfaceSnowProgram`** (new, alongside `gSSSurfaceWetProgram`): reads `ssFieldAt`'s snow
  channel (already returned, already gated to the column top), and for snow-covered cells:
  - **Albedo lift** toward the snow tint with depth, keeping the underlying albedo's shade
    relationships - "caked snow texture over geometry", applied in lighting space so every light,
    probe and projector sees one consistent gbuffer, exactly the reasoning the wet pass documents.
  - **Roughness/gloss** dials: fresh snow is rough and bright; the existing
    `spec_dim`/cloud-transmittance logic the wet pass uses for rain carries over so specular
    behaves under the same deck.
  - **Normal treatment**: the field's slope/edge data (already in `mWindowFlowData`) plus
    procedural flake noise leans the shading normal - fresh snow fuzzes the surface, packed snow
    (below) keeps some of the ground's relief.
  - **Glints**: the RDR2/Crysis sparkle is a per-fragment hash on the sun-facing half vector -
    a cheap screen-stable hash (the `ssFieldHash` pattern) gated on depth and view alignment,
    folding into the specular term. Off-axis it vanishes, which is what sparkle *is*.
- **Depth gating**: the same `on_top`/exposure logic `ssSurfaceFieldF.glsl` already resolves -
  walls do not wear their cell's snow (only rain-wet is a wall-earned channel), roofs do.

**The 3D read - parallax occlusion mapping.** First instinct confirmed, with one honest caveat.
The snow depth per cell is a real heightfield; inside the snow pass, POM against a procedural
relief (fbm, offset by cell hash for stability) scaled by that depth gives the buried-kerb,
soft-intersection read on every surface the field knows about, at zero geometry cost - the same
stability argument as every other world-anchored trick here (hashes in agent space, camera
independent). The caveat: this is a deferred post pass, so POM **cannot move the silhouette** - the
displacement is shading only, and a 30 cm kerb poking through snow has a real geometric silhouette
no shading will bend. That is fine: the relief is sub-cell (ridges, drift ripples, a footprint
wall), the cell height is what buries things in the *look* of them, and the alternative -
displacing the terrain mesh or generating drift meshes - is rejected here for exactly the reason
the water family stays on stock geometry: the build is arbitrary viewer-side content, and shader-
side displacement over a CPU heightfield is the only representation that works on everything.

What this does *not* attempt: AC Shadows' geometric edge deformation (pushed-out snow ridges as
actual displaced triangles). That needs the surface geometry to exist to displace - terrain
tessellation or per-object decals with edge meshing - and it is the largest cost-for-value item on
the whole list. The POM edge treatment (steepen relief near the snow boundary, darken the compacted
core, lighten the pushed ridge) fakes the read at a tiny fraction of the cost. Deferred, and
written down so it is a decision, not an omission.

**Compaction** (the AC Shadows "compacted areas adhere to movement" note) slots into the field the
same way the avatar capsules slot into the wet pass: avatars compress the cell they stand in
(depth to `compacted depth`, edges of the path keep a slightly deeper lip). One extra channel's
worth of bookkeeping in `Field`, one input to the POM height, one darkening term in the albedo
mix. Follow-on, not launch scope.

## 5. Avatars: caking

`SSAvatarWet` is the template: per-avatar capsule, exposure-driven accumulation, folded into the
surface pass so avatars and the ground agree. The snow version rides the same capsule: while the
preset is a snow type and the temperature is at or below freezing, the capsule gains *caking* on
upward-facing exposure (bias by the rain-shadow exposure the avatar stands in), and sheds - by the
preset's melt figure indoors, or by rubbing off faster than it settles outdoors. The wet pass
already has the "avatar here" containment logic (`ssSurfaceWetF.glsl`); the snow pass reuses it so
ground snow never paints up a body, and the body's own caking is what shows. This is the RDR2
behaviour verbatim: time outdoors in snowy regions cake, indoor time sheds.

## 6. Authoring surface

**Preset additions** (`SSPrecipPreset`, serialised like the rest):

| Field | Meaning | Snow | Blizzard |
|---|---|---|---|
| `mSnowLiftRate` | erosion rate, 0 = the type never blows | low | high |
| `mSnowDepositRate` | lee redeposition rate | low | moderate |
| `mSnowDriftAge` | drift particle life cap, seconds | 2.5 | 3.5 |

Thresholds stay global settings (`SSAtmoSnowLiftLo/Hi`) - they are physics, not art direction, and
one band keeps every snow type consistent about when the wind starts winning.

**Weather state**: the squall is a *derived* label, not a new input - `SSAtmoEnvWeatherResolver`
classifies type and intensity already; a temperature-at-or-below-freezing moisture band at
`TURBULENT`/`SEVERE` convection surfaces as "Snow Squall" in the forecast text and (via the
existing intensity band) drives the whiteout pass's background term and the drift tier's budget
multiplier. Nothing new for an environment author to learn; the existing knobs (moisture,
convection, wind, temperature) compose into it.

**Footsteps** get `STEP_*_SNOW` surfaces the way wet/puddle did, keyed off the field's snow
channel at the foot - the enum, the global-setting plumbing and the surface-name tables all
already enumerate the pattern. Follow-on with the sounds, not launch scope.

## 7. Settings

New keys, in the house style (`SSAtmoSnow*`):

- `SSAtmoSnowLiftLo` / `SSAtmoSnowLiftHi` - the 3-8 m/s band, defaults 3.5 / 8.
- `SSAtmoSnowLiftResponse` - how fast drift particles take up the field (analogous to
  `SSAtmoWindFlowParticleResponse`).
- `SSAtmoSnowDriftBudget` - share of `SSAtmoParticleBudget` the drift tier may take.
- `SSAtmoSnowSurfaceStrength`, `SSAtmoSnowDepthFull`, `SSAtmoSnowSparkle` - the surface pass
  dials, mirroring the `SSAtmoWet*` family.
- `SSAtmoWhiteoutStrength`, `SSAtmoWhiteoutBand`, `SSAtmoWhiteoutRange`,
  `SSAtmoWhiteoutCorridor` - the fog pass, with the corridor ratio and the band height.
| `SSAtmoSnowDebug` - Debug floater styles, matching the wind flow debug family: 0 off, 1 the
  erosion/deposit field (red scouring, blue banking), 2 the lift rate per cell, 3 the whiteout
  density.
- `SSAtmoSnowDensityRes` - the air layer's texture resolution (128 default; a quality-tier
  lever, not an art dial).
- `SSAtmoSnowRegimeOverride` - -1 auto, 0-4 forces a regime (debug and screenshots).

Everything above degrades gracefully when `SSAtmoWindFlow` is off (or the GL 4.3 requirement
fails): `sample()` falls back to the ambient vector, so the threshold simply becomes an ambient
one - drift starts uniformly rather than in the corridors, and whiteout loses its spatial
structure but keeps its squall term.

## 8. Phasing

The concrete code architecture - files, classes, signatures, data flow and per-phase touch lists -
is in `doc/atmo_magic_snow_architecture.md`.

1. **Erosion + drift particles** - the field branch, the spawn gate, the RISER parameterisation.
   Purely additive; the visible change is snow moving with the wind from the ground up. Smallest
   change, biggest single effect.
2. **Redeposit** - the lee/stagnation term in `tick()`, so scouring and banking emerge from the
   same wind that moves the particles.
3. **Snow surface pass** - albedo/normal/specular/sparkle over the existing field window; the
   moment snowfall starts looking like snow on the ground rather than a debug quad.
4. **Whiteout pass** - local fog with the corridor term; the squall derivation feeding it.
5. **Avatar caking, footsteps, compaction** - the tactile follow-ons, each riding a pattern that
   already exists.
6. **Granular runoff** - creep advection, the shed re-feed, dithered cascades (section 13); needs
   the surface pass to scroll and the field to feed, and is the doorway to the sand skin.

## 9. Performance, quality and LOD

### The cost inventory

Nothing here adds a GPU solve. The wind flowmap is already solved, staged and read back
asynchronously per region; every new piece is either CPU work inside existing throttled ticks or
fullscreen shading passes in the wet-pass family. Per feature:

| Feature | CPU | GPU | Idle cost |
|---|---|---|---|
| Erosion/deposit | one throttled `tick()` pass per region (`TICK_INTERVAL`, `MAX_TICK_DT` budget, shed cursor - the wet/puddle model unchanged), plus one wind fetch per snow-holding cell | none | zero - cells with `mSnow == 0` skip the fetch, no lift active means no work |
| Drift particles | a slice of the existing `SSAtmoParticleBudget` sim; riser lifetimes are 2-3 s, so the standing population is spawn rate x life | the precip renderer's existing buckets, one more material bucket | zero when no cell passes the lift gate |
| Snow surface pass | none | one fullscreen pre-lighting pass (+ its commit draw) | zero - gated on `peakSnow() > 0` and the field window, exactly like `renderWetPass` gates on wet strength |
| Whiteout pass | none | one fullscreen post pass, ~8 texture fetches (depth, colour, field window, flow window, 3-4 band taps) | zero - gated on lift activity + squall intensity |
| Granular runoff | the shed cursor already walks edges within a fixed visit budget; creep adds one neighbour exchange per ticked cell | the precip renderer's existing stream/drip buckets, dithered branch | zero - the shed accumulator only fills while creep or spilling feeds it |
| Air density layer | none - targets ride the ground window upload | two ~128² mini passes per region (advect + relax) | zero - gated on lift activity |
| Regime machine | a few scalars per fixed tick | none | zero |
| Avatar caking / footsteps | per-avatar idle bookkeeping (`MAX_SHADED` = 8) and point field samples | rides the existing passes | zero |

Two fetch-cost notes that are design decisions, not implementation details:

- **The field tick must not call `SSWindFlowMap::sample()` per cell.** `sample()` runs `gustAt()` -
  fbm calls - per invocation, and a region grid is thousands of cells. Gusts vary over ~100 m
  wavelengths while the field cell is well under a metre, so gust structure is lost at field
  resolution anyway: add a `sampleGround()` to `SSWindFlowMap` that bilinears the solved field
  straight out of the CPU-resident `mFlow` tiles with no gust evaluation - picking, per column,
  the first slab whose ceiling clears that column's own surface height, because terrain and
  builds slope across many slabs and no one slab is "the ground" - and apply the gust
  envelope once per region per tick (it is a scalar here, not a per-cell veer). Same answer, an
  order of magnitude fewer trig calls.
- **The whiteout pass runs at half resolution by default.** Fog density is the lowest-frequency
  quantity on screen; a half-res density buffer bilinearly upsampled before the colour blend loses
  nothing visible and halves the fetch bill of the most expensive new pass.

Rough landing zone, assuming the wet family (two fullscreen passes + commits) was accepted: the
snow surface pass is comparable, the whiteout pass is similar-or-cheaper at half res, and the CPU
drift work is bounded by the particle budget the sim already enforces (`tierCap`, headroom,
`RIPPLE_CAP`-style pooling). Net: two additional fullscreen passes when snow is actively falling or
blowing, no new solve, no new capture.

### Quality dial

One enum drives the whole feature set, in the house style of a single setting with named tiers:

`SSAtmoSnowQuality` - 0 off, 1 low, 2 medium (default), 3 high, 4 ultra.

| | Off | Low | Medium | High | Ultra |
|---|---|---|---|---|---|
| Erosion/deposit | off (snow still settles) | on, coarser field | on | on | on |
| Drift tier | off | 24 m band, 8% budget share | 48 m, 15% | 96 m, 25% | 96 m, 30% + raised tier cap |
| Snow surface | off | flat shade, no POM, no sparkle | POM 8 steps within 24 m, sparkle | POM 16 steps within 32 m | POM 24 steps within 48 m |
| Whiteout | off | 2 taps, half-res | 4 taps, half-res | 6 taps, half-res | 6 taps, full res |
| Compaction / caking | - | off | caking only | both | both |
| Granular runoff | off | cascades blended, no creep | dither near + creep | + clump spawn | full |
| Regimes / air layer | off | regimes only | air layer 96² | air layer 128² | + particle splat |
| Field tick cadence | - | relaxed | default | default | default |

The individual strengths already proposed (`SSAtmoSnowLift`, `SSAtmoSnowSurfaceStrength`,
`SSAtmoWhiteoutStrength`, `SSAtmoSnowLiftRate` per preset) remain master dials on top - the enum
sets *how much machinery runs*, the strengths set *how visible it is*, and any strength at 0
removes its pass entirely, so an author who wants accumulation without drift or a blizzard without
the surface look can have each piece alone.

The feature-level gates also carry through: the surface and whiteout passes sit behind the same
deferred-shader availability the wet pass requires, the flowmap dependency keeps its existing
`SSAtmoWindFlow` + GL 4.3 check with the ambient-wind fallback from section 7, and every tier
obeys the `SSAtmoParticleBudget` ceiling the rest of precipitation lives under.

### LOD mechanics

- **POM taper**: step count lerps from the tier maximum down to 2 with distance and cuts to
  normal-only beyond the range above; sparkle cuts out past ~16 m. The expensive part of the snow
  pass is therefore a near-camera effect by construction.
- **Drift spawn band**: mirrors the tier-band pattern (`tierBands`/`tierRadii`) the other tiers
  already use - drift exists inside a radius with cross-fade edges, density scaled by the local
  lift figure, so distant scoured ground keeps its look through the field without paying for
  particles over it. A `SSAtmoLodDrift` multiplier sits beside `SSAtmoLodDrops` and friends for
  the existing per-tier LOD dials.
- **Whiteout band taps** scale with the quality tier; at 2 taps the march is start/end/mid and
  relies on the low-frequency nature of the density field.
- **Erosion tick cadence** reuses the surface field's existing budget: regions tick in
  camera-distance order under one cursor, and a lifted field region spends its tick only on cells
  that hold snow. A heavy blizzard and a light dusting cost the same per cell that has snow; bare
  cells are skipped before the wind fetch.
- **Idle everything**: `peakSnow() == 0`, no lift activity and no squall intensity gate all three
  passes out before they bind a single texture - the dry-summer cost of the whole design is the
  settled-snow tick, which is the sim that already runs.

### Instrumentation

Each phase lands with its `LL_RECORD_BLOCK_TIME` block (`FTM_SS_SNOW_SURFACE`,
`FTM_SS_WHITEOUT`, plus the drift slice inside the sim block), the Debug floater debug views
from section 7 double as the visual verification for LOD cuts, and the ss floater stats line -
which already prints surface ms and peak wet/snow/puddle - gains drift count and lift activity so
a user reporting "snow is slow" reports with numbers.

## 10. Alternatives considered

Sections 1-9 describe one design. It was not the only shape this could take, and it is worth
writing down the other two seriously, attacking each on its own terms, and then taking what
survives. The three designs differ on exactly two axes: **who owns the snow's ground state** (the
CPU field, a GPU volume, or nobody) and **where whiteout lives** (surface fog, air-band fog, or a
real volume).

### Design A - field-coupled CPU everything (sections 1-9 as written)

Erosion/deposit in `SSSurfaceField::tick()`, drift as a RISER flavour, whiteout as a screen-space
band march over a constant 2-3 m slab above the stored surface, snow surface pass + POM in the wet
family. Deterministic end to end, gated to zero when idle, entirely inside existing patterns.

**Adversarial review.**

- *Whiteout leaks.* The band march integrates fog along the ray but never asks whether anything
  occludes a given tap. A wall 5 m from the camera hides a howling corridor behind it - the march
  still adds the corridor's fog. Paradoxically the *dumber* design (Design C's depth fog) is
  occlusion-correct by construction: it integrates density only up to the first surface, and
  whatever the surface hides is not drawn. A has the most expensive whiteout and the leakiest one.
  This is the design's one genuine hole.
- *The mass story is a lie at the particle layer.* Erosion spawns drift, but drift particles never
  deposit; redeposit is a flow-potential term with no knowledge of where particles went. Two views
  of one gate, not a conserved system. Defensible - per-particle deposit means field writes from
  the sim, cross-thread, for a difference nobody can see - but it must be said out loud.
- *Threshold churn.* A cell sitting at the lift threshold can erode and, as its neighbour's lee,
  deposit every tick. Needs hysteresis (deposit threshold strictly below lift), which the body
  never stated.
- Otherwise the review comes back clean: fit, cost, determinism, degradation all hold.

### Design B - GPU density volume

Extend each flowmap tile with a snow-density volume in the existing slab layout, advected
semi-Lagrangian by the solved field every frame, eroded from the field window and settled where the
flow stagnates. Whiteout becomes a true raymarch of that volume: occlusion falls out of the solid
mask, plumes curl over rooflines because the solved vz lifts density into the upper slabs, and a
gust front sweeps a courtyard as a visible fog wave. Drift particles become garnish.

**Adversarial review.**

- *It breaks the house rule the whole stack is built on.* `SSAtmoMagic` is a synced deterministic
  weather manager: shared clock, cell-hash spawns, a flowmap that is "solved once, anchored to the
  region". A running sim is per-viewer state - two avatars side by side see different plumes, and
  the Debug floater debug views draw *the field*, not a divergent local sim. Statistical parity
  is achievable; the philosophy and its debuggability are not recoverable.
- *Split-brain accumulation.* The ground truth the shaders, footsteps and avatars read is the CPU
  field. The volume can own airborne density, but deposit must land in the CPU field - either a
  readback every frame (stall, latency) or a second CPU deposit model, at which point the volume's
  deposit is fake and the CPU field is Design A again with extra steps.
- *The cost argument inverts and then fails.* A per-frame advect over 96 x 96 x 8 slabs is tiny -
  cheaper than one fullscreen pass - so cost is *not* the reason to refuse B. The reasons are
  determinism, state management (eviction mid-storm pops the airborne snow, tiles need margined
  volume continuity across borders at frame granularity), and complexity budget for gains that are
  reachable cheaper (below).
- *What B genuinely wins:* occlusion-correct fog, over-roofline plume fog, spatial gust waves in
  the air. All three are real; the first two have cheaper paths.

### Design C - layered visual-first

Ship the look, skip the coupling. No erosion; snowiness is the settled field. Drift particles spawn
where exposure x ambient speed x a coarse snowiness scalar are high, no per-cell lift rate.
Whiteout is per-pixel depth fog whose density is a function of the *surface point's* cell (the
corridor's far wall fogs hard, the plaza wall does not). Everything independently toggleable.

**Adversarial review.**

- *It fails the stated ask.* The brief says effects are "introduced and added on" as intensity
  scales - scouring ground bare and banking drifts against walls IS the added effect of sustained
  wind. C has no mechanism for the ground to change state at all: a 20-minute ground blizzard
  leaves the world as white as it found it, with the air full of snow. That is the difference
  between a snow *skin* and a snow *system*.
- *Its whiteout is better than it looks and worse than it needs to be.* Surface-cell density fog is
  occlusion-correct and structured on visible geometry - but it fogs *surfaces*, not *air*. The
  depth read of standing in blowing snow - the near air itself thickening, the wall 3 m away going
  soft - is exactly what surface fog cannot produce. C is a snow skin with good manners.
- *Its spawn gate lifts a dusting as hard as a drift* - no depth coupling, so fresh centimetres
  stream at full rate.
- *What C genuinely wins:* the decoupled **near-camera layer**. A's drift exists only where cells
  qualify; stand in a sheltered courtyard mid-blizzard and the storm still ought to be in your
  face - RDR2's close-to-screen streaks are a screen-space garnish, not world particles. C also
  wins robustness: no feedback loops means no tuning cliffs.

## 11. Synthesis

The shipped design keeps A's skeleton and steals B's two reachable wins through cheaper doors and
C's one unanswerable point:

1. **From A, unchanged:** erosion/deposit in the field tick via `sampleGround()`, drift as the
   RISER flavour, the snow surface pass + POM taper, the quality enum, determinism throughout,
   the whole of sections 1, 4, 5, 6, 7, 8, 9.
2. **Amendment to section 1 (hysteresis):** the deposit threshold sits at roughly 70% of the lift
   threshold, so a cell cannot erode and bank in the same tick range, and the deposit term carries
   a per-tick cap. The gate ramp (`smoothstep`) already softens the edge; the gap makes it stable.
3. **Amendment to section 3 (occlusion):** the whiteout band march samples the field window's
   surface Z along the ray - the data is already bound, two to three extra taps per pixel - and
   drops any tap whose ray point is below the captured surface between it and the camera. Air-band
   fog with occlusion: B's correctness at A's cost, from data A already owns. The trichotomy this
   resolves: C fogs surfaces (occlusion-correct, no air), A-as-written fogged air (air, leaky),
   this fogs occluded air.
4. **Amendment to section 2 (loft and plumes):** drift loft is a mix of the preset's gentle climb
   and the flow sample's own vz - rooftops and other high surfaces spawn from their own cells (the
   riser floor resolve already does this), so the snow lifting off a roof streams *above* the
   street's band. The per-cell band height in section 3 inherits the same surface-relative
   definition, which is most of B's "plume over the roofline" without any volume.
5. **From C, the near-camera layer:** a small, capped ring of world-anchored streaks following the
   camera - a few hundred, spawn-gated once per frame on the camera cell's own lift figure plus
   squall intensity, so a sheltered courtyard does not storm at the lens. Purely visual, decoupled
   from mass by design, first thing a quality tier cuts.
6. **Deferred with B's name on it:** a *static* per-slab deposit-potential field - the divergence-
   weighted calm zones per slab, solved once with the flowmap, no per-frame anything - as a later
   refinement of where snow banks. It is B's volumetric idea tamed into the anchored-field
   philosophy; the running sim itself stays rejected (determinism, split-brain, state).

## 12. Adversarial review of the synthesis

Attacking the combination, not the parts:

- *The two whiteout inputs can disagree.* Corridor term comes from the flow solve, occlusion from
  the rain-shadow capture - but both derive from the same captures, so they cannot disagree about
  where the ground is; worst case is a capture-cadence lag, shared with every other consumer of
  both maps. Acceptable, already the house pattern.
- *The near-camera layer lies.* It is gated on the camera cell's lift, but "camera cell" is coarse:
  stand at a courtyard's edge and the cell may qualify while the exact spot is calm. Residual
  artifact: a few streaks where the air is still. The gate is one sample per frame - cheap enough
  to use the precise `sample()` there instead of the coarse figure.
- *The mass narrative is still approximate, now on purpose.* Particles are visuals; deposit is
  flow-potential. The visible consequence - a calm courtyard banking snow no particle entered - is
  where wind would put it anyway. The error direction is *flattering*, not wrong-looking.
- *Complexity creep is the real risk.* The near-camera layer and the static slab potential are both
  "easy to keep adding" items; both are hard-capped - the layer at a few hundred particles and a
  first-cut quality tier, the potential field behind an explicit phase gate. The rejection of the
  running volume is the load-bearing decision; everything deferred stays behind it.
- *What was genuinely given up from B:* spatial gust waves in the fog - as of the first round.
  The second design round (`doc/atmo_magic_snow_review.md`) recovered them with the display-
  density layer: a GPU-relaxed 2D air texture chasing CPU-authored, deterministic targets. What
  remains given up from B: a true 3D volume (sub-band detail, vertical plume structure beyond the
  surface-relative band) and per-viewer identical air swirls - accepted, bounded, and stated as
  the "ground exact, air approximate" rule.
- *What was genuinely given up from C:* its simplicity - kept deliberately, because the ground
  changing state is the brief.
- *Perf:* the amendments cost two to three field taps in the whiteout pass and a few hundred
  particles; section 9's envelope and table gain one row (near-camera layer: first cut, off below
  Medium) and otherwise stand.

The second round's synthesis added two layers on top of everything above - the display-density
layer (section 3) and the regime machine (section 14). Their rules, and the adversarial review of
the combination, live in `doc/atmo_magic_snow_review.md`.

## 13. Granular runoff: creep, eave cascades and the sand skin

Rain already runs off buildings: `buildGeometry` marks every cell with an open side or a drop below
as an edge (`sssurfacefield.cpp:218`), `shedRegion` runs those edges through a store/drain
accumulator (`SHED_DRAIN_TAU`, per-frame visit cursor, `DRIP_BUDGET`, `SSAtmoRunoffRadius`
culling - `sssurfacefield.cpp:321`), and the accumulator drives `refreshStream` cascades and
`spawnDrip` clumps that land and queue `from_runoff` impacts. None of that machinery is
water-specific: the store is an inflow/drain pair, the streams carry the preset's material and
tint, and the eaves are just edge cells. Granular runoff is a re-feed and a re-skin, not a new
system. The reference behaviour is the sand sheeting across a dark roof and pouring off the lip in
dithered threads: **horizontal drift blended, vertical cascade dithered.**

### Horizontal drift: creep

**Mass.** `tick()` gains a creep advection term alongside lift: each cell pushes a downwind flux
`F = creep_k * lift * depth` along the flow direction into its neighbour. The flux is capped per
tick at a fraction of the source cell's depth - a CFL-style guard, so advection can never move
more than exists and the step cannot oscillate. Arriving mass piles against the same `lieHere()`
repose room that snowfall uses; when a cell's room is full the flux passes over it, and when the
cell it is entering is an edge cell the arriving flux is the shed accumulator's inflow. This one
term is what turns the drift field from a static skin into a moving surface: sheets migrate
downwind, thicken where the flow stalls, and thin out over exposed crowns.

**Look.** The surface pass gets a granular scroll: the wet pass's flow-window animation path
(`ssSurfaceNormalF` advects wave normals along `mWindowFlowData`) re-purposed as an *albedo*
detail scroll - a stretched ripple noise advected along the flow vector, coverage-lerped into the
snow albedo. That is the picture's smooth moving sheet, and it stays blend at every distance,
because it is coverage on a surface in a deferred pass, not geometry in the alpha queue.

### Vertical cascade: the eave

`shedRegion` runs unchanged with a granular feed: inflow = arriving creep flux plus over-repose
slumping (a steep cell holding past its repose line sheds straight into the store), drain
produces cascades through the same `refreshStream`/`spawnDrip` calls. The differences are the
preset's own: slower scroll, clumped texture, width from `mStreamSpan`, colour from the tint.
Drips land through the same `resolveColumn` the rain uses - and for granular material, landing
**deposits**: the clump's mass is credited to the landing cell, capped by repose room, and
overflow re-enters that cell's store. A cascade that lands on a ledge cascades again; the
multi-stage pour in the picture is emergent, and the eave drift pile at the wall's foot - the most
recognisable snow shape there is - forms itself.

One ledger rule keeps the books honest: the lip cell is debited when the store ingests, the
landing cell is credited on impact, and the store itself stays a rate limiter exactly as it is for
rain - never a second mass pool.

### Dithered transparency on the cascade

The precip particle fragment shader gains a dithered branch for granular materials: when the
particle's material flag says granular, alpha becomes a screen-hash test - `hash(gl_FragCoord.xy)
< alpha` discards, otherwise the fragment writes opaque with depth. Static 4x4 Bayer or
interleaved gradient noise, deliberately **not** frame-rotated.

- *The stipple is the material.* A thin cascade of sand or powder is partial coverage; a static
  stipple reads as grains, where alpha blending reads as a glass sheet of liquid. The quad moves
  across a fixed screen pattern, which reads as grains passing - the desired crawl.
- *Sorting for free.* Depth-writing cascades composite correctly against flakes, drift, other
  cascades and the whiteout pass (drawn before weather) with zero sort work.
- *Blend crossfade with distance.* Stipple density is constant in screen space, so past ~20-25 m
  the dither crossfades back to plain alpha - stipple aliasing grows exactly as coverage detail
  stops mattering.
- Temporal-hash dithering (per-frame rotation) was considered and rejected: it shimmers on a
  slow cascade, and the static pattern's apparent grain motion is already the effect.

### The sand skin

Nothing above is snow-specific. The field channel, creep, shed, cascades, whiteout and footsteps
are **granular transport wearing a preset**. A "Sand" built-in - FLAKE archetype, sand tint,
repose 34 degrees, `mSnowMelt` zero (wind ablation is the only loss; the melt slot is the
granular loss slot), thresholds on the same global 3-8 m/s band, which brackets fine sand's
onset - turns the entire stack into the screenshot: creep sheeting across surfaces, dithered
threads over every lip, banking where the flow dies. Whiteout re-tints into a dust veil, the
ground blizzard's steady state becomes a haboob, and dune slip faces are the repose-shed cascade
running on terrain edges. The code keeps its `mSnow*` names; the docs call the channel what it
is.

### Budgets, settings, phasing

Reused as-is: `SSAtmoRunoff`, `SSAtmoRunoffScale`, `SSAtmoRunoffRadius`, `SHED_VISIT_PER_FRAME`,
the visit cursor and the radius culling. New: `SSAtmoSnowCreep` (advection rate),
`SSAtmoSnowCascadeDither` (0 = blend, 1 = full stipple, default ~0.8), and a granular share of
`DRIP_BUDGET` (clumps cost less than rain drips - no ripple pool work on landing). Quality row
(section 9): runoff off / blend-only cascades / dither near + creep / + clump spawn / full. This
is phase 6, after the surface pass it scrolls and the field it feeds.

Adversarial residue, stated: the CFL cap is load-bearing (without it a gust front stripping a
roofline overshoots and machine-guns the store); creep must be slope-gated (walls hold their
exposure-driven film and do not creep sideways); and the dither's one honest cost is a faint
static pattern visible on a perfectly still cascade at close range - accepted, because a perfectly
still cascade of granular material at close range is a few frames from being a pile.

## 14. Regime choreography

Intensity scaling needs *timing*, not just magnitudes: blizzards have beginnings, squalls arrive,
lulls let the fog lift before the next front. A regime state machine over shared time provides
that shape: CALM -> SALTATION -> DRIFT -> BLIZZARD -> SQUALL, entered on the env resolver's own
outputs (ambient speed, snow presence, falling intensity, temperature) and exited with hysteresis
gaps and minimum dwell times (~20-60 s). Each regime is a bundle of scalars, not new machinery:

- gust envelope shape (the existing `gustDepth`/`gustLength`/`gustVeer` parameterisation),
- the near-camera ring rate multiplier (drift spawning itself stays physics-gated - the field's
  lift decides it, per the rule below),
- the whiteout ramp rates (squall onset collapses visibility over ~8 s; the lift after one takes
  ~20 s - when a squall onset collides with a gust spike the pass takes the `min` of the two
  demand curves, never the sum),
- an audio bed crossfade trigger and the packed-vs-fresh surface look.

Three rules keep it honest. **Derived, not authored**: regimes come from the same params the
weather resolver already produces (per-preset overrides are the only authoring surface), so they
cannot fight the environment track. **The regime directs, the field decides**: regimes scale
presentation and global envelopes only - local lift never reads the regime, so a corridor jet
still saltates in CALM, it just does not drag the storm's fog and audio with it. **Fixed-step
transitions**: regime changes evaluate on the same shared-time tick boundaries the transport
steps on, with the initial regime a pure function of current params, so a viewer joining
mid-storm converges at the next boundary rather than replaying history.

## Deferred / known limits

- **POM in a deferred pass shades, never silhouettes.** Geometric burial (a kerb genuinely
  swallowed, AC Shadows' pushed ridges) needs surface geometry to bite into; see the rejection
  above. The field's depth, the repose gate and the POM relief read correctly on slopes, drift
  banks and footprint edges regardless.
- **The drift band is a band, not a volume.** Section 11's amendments make the band occlusion-
  correct and surface-relative, so plumes ride rooflines and fog stops at walls, but the air
  between band top and cloud deck carries no density of its own: a gust front still does not sweep
  one street as a visible fog wave while the next stays clear (see section 12). The static
  per-slab deposit-potential field is the sanctioned next step if the band ever reads wrong; the
  running density volume stays rejected.
- **Redeposition is per-region and terrain-anchored**, like everything in `SSSurfaceField`: a
  drift banks against the *field's* stored surface, cell-resolution. A fence finer than the cell
  catches snow coarsely. The field cell size is the accuracy budget, as it is for puddles today.
- **Drift responds to flowmap rebuilds instantly.** A building appearing mid-storm moves the jet,
  and the snow that was banking behind the old shadow line is already somewhere else - the field
  re-erodes and re-deposits on its own tick budget, but the first minutes after a big edit can
  reshuffle visibly. The edit-settle machinery (`SSAtmoMagic::settleEdits`) already gates the
  captures; the field's `REBUILD_DZ` reset is the analogous damper here.
- **Cross-region drift stops at region borders** like everything else region-anchored; the
  flowmap's own margin overlap keeps the *wind* continuous, and particles crossing a border
  simply continue on the next region's field, but the accumulated bank at a border is not shared
  state.
- **Sky tracks** work the same way rain does today - `resolveColumn()` finds platform tops, snow
  settles on decks, and drift will scour them if the wind says so; there is no "indoor snow"
  concept beyond exposure, which is the correct one.
