# Atmo Magic — cross-system interaction map

> **OUT OF DATE** — archived 2026-08-26. The `[interaction: ...]` tags at the use sites in code are the ground truth.

Who reads what. When changing a system, check its consumers here — the point of this file is knowing the blast radius before touching anything. Comments tagged `[interaction: ...]` in code refer back to this map. May drift out of date; the tagged comments at the use sites are the ground truth.

## Shader variant system (SS_ATMO)
Any Atmo-era change to SHARED shading (stock sky, dome clouds, the windlight atmospheric module) lives behind `#ifdef SS_ATMO` with the stock line in the `#else`. The define is injected by `add_common_permutations` (llviewershadermgr.cpp) only while `SSAtmoEnabled` is on, and toggling that setting triggers a full shader rebuild via its `handleSetShaderChanged` listener — with Atmo off, every stock shader compiles byte-for-byte pristine, zero runtime cost either way. CPU twins of shader variants (lllegacyatmospherics' Chapman cap) gate on an `SSAtmoEnabled` LLCachedControl to stay in lockstep. Runtime nuance (weather active vs idle) stays uniform-driven inside variant blocks; the define is only the master pristine/modified split. Current occupants: the far-field water squash (waterV) and the rain sun-refraction/scatter tuning. A cautionary former occupant: a "Chapman cap" on the horizon sunlight cosecant, built to fix sunset glow dying at the horizon — reverted once the real cause proved to be AUTHORING, not math: near the horizon the sky's additive collapses to (blue_horizon x blue_weight + haze_horizon x haze_weight) x AMBIENT, so the horizon band IS the authored ambient colour; great SL sunsets author warm ambient/blue-horizon at the sunset keyframes and the band becomes the glow. Fix presets before fixing equations.

## Producers and their consumers

### SSWindFlowMap (sswindflow) — solved 3D wind field + per-column height capture
- **Precipitation** (`ssprecipitation.cpp windAt()`): drop drift, stream/drip bending, shard scatter.
- **Runoff streams** (`refreshStream`): bent by `windAt(lip)` — local, not global wind.
- **Footsteps** (`sssoundscape footstepSound`): the overhead height capture answers "indoors?" at each avatar's feet (`surfaceAt`).
- **Rain-on-roof audio** (`sssoundscape`): burial depth from the height capture; exposure for outdoor openness.
- **Lightning attachment** (`sslightning spawn`): `forEachColumn` scores height − lateral×bias to land strikes on towers/ridges.
- **Wind stereo** (`sssoundscape updateLoops`): wind loop sources sit 6 m upwind of the head, sampled from the local flow.
- **Viewer-wide wind** (`llappviewer`, `llflexibleobject`, `llviewerpartsim`): gWindVec and flexi/particle wind come from the flowmap when solved.
- ⚠ Upgrading the capture (resolution, what counts as a surface) changes footstep indoor detection, thunder burial, lightning landing points and stream bending at once.

### SSSurfaceField (sssurfacefield) — wet / snow / puddle / height per cell + flow map
- **Wet shading** (`ssSurfaceWetF.glsl`) and **normal flatten** (`ssSurfaceNormalF.glsl`): read the stitched window; both must agree on "is this a puddle" (shared `ssWetFlattenCos*` uniforms and `ssWetPuddleDepthFull`).
- **Footstep surface pick** (`sssoundscape footstepSound`): wet > 0.3 → wet slots, puddle > 5 mm → puddle slots. Terrain vs prim comes from `onObject()`'s own cached foot ray against render geometry (`lineSegmentIntersectWorldGeometry`), NOT from `mStepOnLand` — the stock resolver behind that flag never assigns its object out-param, so it has read "land" everywhere since the Havok era.
- **Avatar wetness** (`ssavatarwet`): seeds soak from ground state; ground/body blend in the wet shader.
- **Roof runoff**: lip reservoirs live in the field; streams/drips shed what drains.
- **Puddle patch mask** lives inside the accumulation tick — changing cell size or region origin changes where pools form (deterministic per region, same for all viewers).
- ⚠ The rain shadow feeds exposure into this field; a rain-shadow upgrade changes wetting, puddles, footsteps and avatar soak downstream.

### SSAtmoMagic (hub) — per-frame cached params
- `turbulence()` → lightning intervals (v2 fallback), cloud churn, gust depth. `windXY()/wind()` → everything above. `temperatureC()` → thunder sound speed; future freezing. `lightningIntervalMin/Max/Intensity/Color/CoreWhite`, charge/sparks flags → SSLightning + renderer. `isEnabled()` (weather running) vs `isSwitchedOn()` (system on at all): dry footsteps gate on the latter — they play precisely when no weather runs.

### SSLightning (model) → consumers
- **SSLightningRender**: ribbons per stroke (`mStrokeAt/Bright` + wind = ribbon lightning), leader crawl via `mReachedAt` vs `mLeaderProgress`, charge sparks via `mCharge`, impact sparks from `mT` hashes, sky-flash discs from `mFlash`.
- **SSVolCloud**: strikes become `ss_strike[]` point lights in the puff shader (sheet lightning IS this). Field's band also feeds back: channels span `cloudBaseZ()..cloudTopZ()`.
- **SSSoundscape**: `scheduleThunder(pos, dist, intensity, fire_at)` at build time (10 s early), soundscape works backwards from fire time.
- **pipeline.cpp renderDeferredLighting**: `sceneLights()` appended to `fullscreen_lights` — strikes light avatars/wet ground through the ordinary local-light pass, back of the queue, max 4.
- ⚠ Changing strike timing (`mT`, stroke arrays) touches all four consumers; the renderer and cloud lighting assume `mT<0` = not yet fired.

### SSVolCloud — puff field
- ALL weather passes (lightning flash, puffs, bolts, precip) run inside `renderGeomPostDeferred`'s pool loop beside `doAtmospherics()` — BEFORE the alpha pools, so transparent surfaces blend over weather (the transparency test). Trade: rain between the camera and a window reads as behind the glass. They were previously after all pools, which composited weather over every nearer transparent object.
- Renders BETWEEN SSLightningRender's two passes in `renderGeomPostDeferred`: after `renderFlash()` (discs stay veiled under the puffs), before `render()` (ribbons draw over the puffs, each node dimmed by `transmittance()` against the actual puff spheres — `SSAtmoLightningOcclusion` scales the bite). Reordering breaks the compositing silently.
- `SSLightning::idle` runs BEFORE `SSVolCloud::update` in the manager idle — the deck's flicker reads this frame's strokes, not last frame's.

### SSSoundscape — everything audible
- Gates: ambient beds + thunder-in-flight on `isEnabled`; footsteps on `isSwitchedOn`; everything on `SSAtmoSounds`. Pending thunder is serviced BEFORE the enabled gate — a clap in flight lands even if the sky clears.
- `windCarryGain(pos)` is a general offer (downwind carries, upwind muffled, grows with km); thunder is the only consumer so far.
- `skyOcclusion()` (cover 55% + burial 45%) rides every sky-borne one-shot (thunder, charge) as an engine-level LOWPASS (`LLAudioSource::setOcclusion` -> FMOD setLowPassGain, FMOD_INIT_CHANNEL_LOWPASS): occlusion is timbre first, volume second (mass law). Sampled at play time. Beds stay on asset-substitution crossfade instead - roof beds ARE the indoor rendition.
- Speed of sound = 331.3 + 0.606·`temperatureC()`.

### SSSoundMeta — pre-analysed sound metadata
Gathers every configured pack (thunder/charge/wind/beds/footsteps, preset + globals) when Atmo switches on; decodes two at a time, PCM-copies on main (FMOD never leaves the main thread), analyses on a small worker pool. Per sound: length, onset, tail (content end — future chain join point), peak level (levelling), impact rate (future rain-bed auto-match against live `impactRate()`), crackiness (ZCR brightness — future crack/rumble auto-sort). Thunder timing + levelling read this table first, buffer path as fallback. Overlay: `analysis N ready / M pending`.

### Glow buffer (viewer-wide gotcha)
- The screen target's **alpha channel is the glow source** (`glowExtractF.glsl` reads `col.a`). Passes that must not bloom mask alpha off (`setColorMask(true,false)`: volcloud, precip); the lightning pass deliberately writes alpha (`SSAtmoLightningGlow`) and is the only Atmo pass that does. Any new late pass must decide this explicitly.

### Shared twilight model
The celestial discs (`ss_daylight`, lldrawpoolwlsky.cpp) and the volumetric puff lighting (ssvolcloud.cpp update) use the SAME twilight curve: smoothstep over sun altitude −6°..+9°. Changing one band without the other desyncs when clouds darken vs when stars/moon appear. EEP's getSunlightColor/getAmbientColor are AUTHORED constants, not current light — anything CPU-lit must apply this attenuation itself.

### Stock water sizing (LLWorld::updateWaterObjects)
- Sizing is fully STATIC against the constant projection far plane (`MAX_FAR_CLIP` 2048, llcamera.h): hole range fixed at 256, edge skirt = clamp(corner_room, 256, 1024) with corner_room = 0.7·MAX_FAR_CLIP − max(wx, wy) (0.7 ≈ 1/sqrt(2), so the square ring's far CORNER is what fits, from a camera anywhere in the box). Every draw-scaled formula tried before found a regime that ballooned past the far plane, where the projection slices the tiles and the sliced triangles rasterise black along the horizon; against a constant far plane draw-distance changes rebuild nothing.
- The 256 skirt floor is structural: `LLVOWater::updateGeometry` rounds a sub-128 m patch to zero quads, whose half-created empty vertex buffer left a dangling pointer in the mapped-buffer flush list (access violation under `movePartition`); updateGeometry also clamps to 1 quad minimum as a belt.
- llvosky kills the celestial disc the instant the body's CENTRE sets (`getIsSunUp`), which reads as a half-set sun popping out of existence. With Atmo on, a −0.10 z grace band lets the disc keep drawing while it slides below the horizon — water and terrain occlude the sunken part by depth, so the extension costs nothing.

### Depth layering constants
- Skybox passes flatten to ndc z: dome clouds 0.99998 (`cloudsV.glsl`), celestial discs 0.99999 (`ssCelestialV.glsl`) — clouds in front of moon/stars. `LL_SHADER_CONST_CLOUD_MOON_DEPTH` exists but nothing reads it.

## Env (v3) → runtime path
`SSAtmoEnvWeather` (authored, keyframed) → `SSAtmoEnvWeatherResolver::resolve` → `SSAtmoEnvWeatherState` → `SSAtmoEnvBridge::resolveActiveTrack` → `SSAtmoTrackConfig` → `SSAtmoMagic::refreshParams` caches → accessors. Anything added to the weather schema must be carried through all five hops or it silently stays at default at runtime (the lightning bools/colour/temperature all follow this path).

## Preset vs global assets
`SSFootstepSounds::surfaceIsGlobal()` is the single authority on which footstep slots live in gSavedSettings (`SSAtmoStep*`) vs the preset; the preset editor, global assets floater, avatar lookup and preset serialization all ask it. Old presets migrate dry slots into empty globals once on load (`fromLLSD`), then shed them on save.
