# Rendering frame pipeline

All paths under `indra/newview` unless noted. See [glow_and_alpha.md](glow_and_alpha.md) for alpha semantics and `doc/render_glow_pipeline.md` for the verified glow trace.

## Frame order — `display()` (llviewerdisplay.cpp:504), deferred path

1. Early-outs (resize/picking/teleport) → dynamic textures, hero probe (:871-889).
2. `display_update_camera()`, env update, `updateGeom` (:908-926).
3. **`gPipeline.updateCull()`** (:958 → pipeline.cpp:2648) — CPU frustum + VO-cache cull into global `sCull`. Occlusion *queries* are issued later, mid-geom.
4. **`generateSunShadow()`** (:983 → pipeline.cpp:11133) — 4 sun cascades + 2 spot, depth-only.
5. **SS hooks** (:985-1012): `SSAtmoMagic::idle()`, `SSAtmoEnvApplier::apply()`, `SSWaterWorld::update()`, `SSRainShadowMap::capture()`, `SSWindFlowMap::update()` — deliberately right after shadow gen (same pipeline state).
6. Avatar impostor updates (:1021 — hijacks viewport, restores by hand).
7. Image updates, GLTF material flush → **`stateSort()` + `rebuildPools()`** (:1088-1098).
8. Bind `mRT->deferredScreen`, **clear to magenta (1,0,1,1)** — magenta on screen = "nothing wrote here" (:1174-1184). Optional depth pre-pass (:1196).
9. **`renderGeomDeferred()`** (:1219 → pipeline.cpp:4250) — G-buffer fill. Pool loop over `mPools` (set ordered by pool type). **`doOcclusion()` queries issued mid-loop at the first pool ≥ POOL_GRASS** (4320) — that's why opaque pools must sort first in the pool enum. Exits with `setColorMask(true,false)` — alpha writes OFF (4377).
10. **`renderDeferredLighting()`** (:1241 → pipeline.cpp:9616):
    - SSAO/shadow lightmap into `deferredLight` via `gDeferredSunProgram` (9666); SSAO blur **uses `mRT->screen` as scratch** — screen is clobbered then re-cleared (9701-9767).
    - **`SSSurfaceField::renderWetPass()`** (9761) modifies the G-buffer before any lighting reads it.
    - Bind `mRT->screen`, clear (0,0,0,0) — comment: "zeroing alpha (glow) is important" (9765).
    - `gDeferredSoftenProgram` fullscreen resolve (linear HDR out, alpha=0).
    - Local lights additive (`BT_ADD`): point/spot/multi-light batches (9807-10098); **SS lightning scene lights injected at 9992-10027**.
    - Render-type mask narrowed to alpha/fullbright/glow/water → **`renderGeomPostDeferred()`** (10139).
11. `render_ui()` (:1254) → **`renderFinalize()` is called from INSIDE render_ui** (llviewerdisplay.cpp:1713), then HUDs, 3D UI, 2D UI → swap.

### `renderGeomPostDeferred` (pipeline.cpp:4389) — forward/alpha stage

Pool loop with depth-keyed injections:
- `POOL_WATEREXCLUSION` → `doWaterExclusionMask()` (4447)
- `POOL_ALPHA_POST_WATER` → `doAtmospherics()` (4453), then the **SS weather block** (4461-4471): flash → volumetric clouds → lightning → precipitation. The block restores `BT_ALPHA` + `setColorMask(true,false)` because weather shaders trample them — **any new pass inserted here must do the same**.
- `POOL_ALPHA_PRE_WATER` → `doWaterHaze()` (4477)

### `renderFinalize` (pipeline.cpp:9133) — post chain

HDR path: `copyScreenSpaceReflections` → `generateLuminance` (256² R16F, mipped) → `generateExposure` (1×1) → `tonemap` (→ `deferredLight` when CAS on, else `mPostPingMap`) → `applyCAS` (also does gamma). Non-HDR: `gammaCorrect`. Then `generateGlow(mPostPingMap)` → `combineGlow` → DoF → FXAA or SMAA → vignette/snapshot frame → final blit to default FB (must be last stage, 9281). Everything after tonemap is **8-bit gamma space**; glow extraction operates on gamma-space color. Glow buffers are 512×glow_res, non-square (pipeline.cpp:1472).

## Render targets (alloc: `allocateScreenBufferInternal` pipeline.cpp:919; non-per-frame: `createGLBuffers` :1455)

`mRT` points at `mMainRT` / `mAuxillaryRT` (reflection probes) / `mHeroProbeRT`.

| RT | Format | Content / alpha meaning |
|---|---|---|
| `deferredScreen` att0 | RGBA + depth | albedo (sRGB legacy, **linear** PBR); **a = legacy emissive scalar**, 0 for PBR/simple |
| att1 | RGBA | legacy spec color / PBR ORM; **a = legacy glossiness** |
| att2 | RGBA16/RGB10_A2 | XY encoded normal, Z env intensity; **a = gbuffer FLAG** (SKIP_ATMOS 0, HAS_ATMOS .34, HAS_PBR .67, HAS_HDRI 1.0 — float compare ±0.1, llshadermgr.cpp:652) |
| att3 emissive | RGB16F, **only if `RenderEnableEmissiveBuffer`** (default OFF) | PBR emissive; shaders guard `#if defined(HAS_EMISSIVE)` |
| `screen` | **RGBA16F always** | lit scene, linear HDR; **a = glow accumulation** |
| `deferredLight` | RGBA16F/RGBA | `.r` sun shadow, `.g` AO; later reused as tonemap intermediate |
| `mWaterDis` | screen fmt + depth | triple-duty scratch: water refraction + depth scratch for atmospherics/haze |
| `mPostPingMap`/`Pong` | RGBA8 | post chain is LDR after tonemap |
| shadow[0..3], mSpotShadow | depth-only | size rounded to ×16 |
| mLuminanceMap/mExposureMap | R16F | eye adaptation |

**Depth sharing**: `deferredScreen.shareDepthBuffer(screen)` (pipeline.cpp:995). Alpha passes depth-test against G-buffer depth for free; clearing depth on `screen` destroys G-buffer depth.

## Draw pools

Enum order = render order (lldrawpool.h:49-80): SKY … SIMPLE/FULLBRIGHT/BUMP/MATERIALS/GLTF_PBR/TERRAIN … GRASS (occlusion boundary) … ALPHA_MASK/AVATAR/GLOW/ALPHA_PRE_WATER/WATER/ALPHA_POST_WATER/ALPHA. A pool participates in a stage only if its `getNum*Passes() > 0` (deferred / post-deferred / shadow / plain `render`). Materials pool is deferred-only; alpha/water/glow pools are post-deferred.

## Culling/occlusion

`updateCull` (2648): water clip plane, per-region per-partition `LLSpatialPartition::cull`, VO-cache cull; sky pushed unconditionally. `doOcclusion` (2811): GPU queries per `LLSpatialGroup` + probe cube queries, colormask off throughout. `generateSunShadow` disables occlusion via `LLDisableOcclusionCulling` RAII.

## Gotchas

- **State leakage is the norm**: post-deferred ambient state is `setColorMask(true,false)`; `doAtmospherics`/`doWaterHaze` flip colormask AND set a custom blendFunc `(ONE, SRC_ALPHA, ZERO, SRC_ALPHA)` and restore only the blend type, not the colormask (10196, 10260).
- `doAtmospherics`/`doWaterHaze` **flush and re-bind `mRT->screen` mid-pass** to copy depth into `mWaterDis` — don't hold bound-target assumptions across them.
- `SSSurfaceField::renderWetPass` (sssurfacefield.cpp:919) rebinds `deferredScreen` and uses raw `glDrawBuffers` to write only attachment 1 (and optionally 2); if the restore of all four draw buffers (:1384-1386) is skipped, all later G-buffer writes silently vanish.
- `tonemap` reuses `deferredLight` as scratch when CAS is on — its shadow/AO content is dead by then.
- `addDeferredAttachments` silently no-ops if the target already has >1 texture (pipeline.cpp:397).
- `setSkipRenderFlag` is honored in the deferred loop (4349) but NOT post-deferred (4503).
- `renderDeferredLighting` toggles off RENDER_TYPE_HUD and never restores it in scope (9641).
- `RenderResolutionDivisor/Multiplier` scale all screen RTs; shadows have independent `RenderShadowResolutionScale`.
- SS shader programs: 15 `gSS*Program` globals (llviewershadermgr.cpp:183-200); shared-shader edits live behind the `SS_ATMO` define (see [soapstorm_layer.md](soapstorm_layer.md)). Shader binary cache is keyed on source digest, not viewer version (llviewershadermgr.cpp:567-629).
