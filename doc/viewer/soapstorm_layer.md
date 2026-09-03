# The Soapstorm layer (ss* files)

All fork-added code lives in `indra/newview/ss*.{h,cpp}`, tagged `<SS:Nexii>` in shared files. Comments are single long lines; cross-system dependencies are marked `[interaction: SystemName]` at the use site — those tags are the ground truth for blast radius (the archived interaction map `doc/archive/atmo_magic_interactions.md` may drift).

## The SS_ATMO shader variant rule

Any change to SHARED stock shading (sky, dome clouds, windlight module, water) goes behind `#ifdef SS_ATMO` with the stock line in `#else`. The define is injected by `add_common_permutations` (llviewershadermgr.cpp) only while the `SSAtmoEnabled` setting is on; toggling it rebuilds shaders. With Atmo off, stock shaders compile byte-for-byte pristine. CPU twins of shader math gate on an `SSAtmoEnabled` LLCachedControl. Runtime nuance (weather active/idle) stays uniform-driven inside the variant block — the define is only the pristine/modified master split.

## Systems

Simulation/state (mostly LLSingletons, ticked from idle/update paths):

- **SSAtmoMagic** (`ssatmomagic.*`) — weather director; owns precip sim wiring, drives the rest.
- **SSAtmoTrack** (`ssatmotrack.*`) — weather cell tracks moving across the world (`doc/archive/atmo_magic_tracks.md`).
- **Atmo Env authoring** (`ssatmoenv*`) — environment asset format (`ssatmoenvasset`, ~50K), applier onto stock EEP sky (`ssatmoenvapplier`), manager, bridge, discovery, keyframes, planetary/weather/cloudfield/track state, sky modulator. Authoring UI: `ssfloateratmoenv` (75K), `ssfloateratmoplanetary` (60K), presets `ssfloaterpreset`.
- **SSPrecipSim / precipitation** (`ssprecipitation`, `ssprecippreset`, `ssprecipvariants`, `sspreciprenderer`) — rain/snow particle simulation + its renderer.
- **SSRainShadowMap** (`ssrainshadow.*`) — top-down occlusion capture so rain doesn't fall indoors.
- **SSWindFlowMap** (`sswindflow.*`, 89K — largest) — wind flow field around geometry (`doc/archive/atmo_magic_windflow.md`).
- **SSSurfaceField** (`sssurfacefield.*`) — surface capture for runoff/wetness (`doc/archive/atmo_magic_runoff.md`).
- **SSVolCloud** (`ssvolcloud.*`) — volumetric cloud field. Far-field depth squash cap shared with lightning so bolts and clouds agree on drawn depth.
- **SSLightning / SSLightningRender** — bolt simulation + rendering (`doc/archive/atmo_magic_lightning.md`; the ground strike's aura, amber, plasma, crawl, sparks and fire in `doc/atmo_magic_lightning_strike.md`, drawn through one per-frame LLVertexBuffer). Bloom via additive alpha into the post-deferred screen RT (see [glow_and_alpha.md](glow_and_alpha.md)).
- **SSAvatarWet** (`ssavatarwet.*`) — avatar wetness response to rain.
- **SSSoundscape** (`sssoundscape.*`, `sssoundmeta`) — weather-aware ambient audio.
- **SSWater / SSWaterWorld** (`sswater.*`, new on this branch) — Atmo-owned water plane objects. Reuses stock water pcodes/pool/shaders wholesale; `LLVOWater::mIsAtmoWater` is the ONLY discriminator, and because pcodes are shared the pcode factory cannot build these — SSWaterWorld news them and hands them to `gObjectList.adoptViewerObject`. See `doc/atmo_magic_water.md`.

Dev/debug floaters: `ssfloateratmo` (weather debug), `ssfloateratmoinfluence`, `ssfloatersim`, `ssfloatersoundlist`/`ssfloatersoundanalysis`, `ssfloatertexturelist`, `ssfloaterassets`/`ssassetlist`, `sspanelprefs`.

## Gotchas

- **Perlin determinism**: sim code must stay deterministic; don't casually reseed or reorder noise sampling (see memory: shared noise fields feed multiple consumers).
- **Far sea was removed** (2026-08-26, commit `9c3a3f56fa`); it lives only in git history. Don't resurrect fragments of it by accident when touching water/horizon code.
- **Docs in `doc/archive/` are marked OUT OF DATE** — use them for intent and rationale, trust code + `[interaction:]` tags for current truth. `doc/archive/atmo_magic_code_notes.md` (745K) is the deep build log.
- The WorldField (`doc/archive/atmo_magic_worldfield.md`) is an UNBUILT proposal to unify the four scene-capture systems (rain shadow, runoff, windflow, audio); it's on the backlog, not in the tree.
