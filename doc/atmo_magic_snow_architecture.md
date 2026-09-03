# Atmo Magic snow: code architecture

The implementation plan for `doc/atmo_magic_snow.md` - files, classes, signatures, data flow and
wiring, in build order. Everything is additive to the existing Atmo Magic singletons; no new
threads, no new GPU solves, and every pass gates to zero when idle. Section numbers in brackets
refer to the design doc.

## Principles carried from the design

- **The surface field stays the one ground truth.** Depth, lift and creep live in
  `SSSurfaceField`; particles, shaders and sounds read it and never write it except through one
  deposit entry point.
- **One lift authority.** "Is snow lifting here and how hard" is computed in exactly one place
  (`SSAtmoMagic::liftAt`) and consumed by drift spawning, runoff feed, the near-camera ring and
  stats. No component re-derives the threshold band.
- **The wind flowmap stays a static, deterministic solve.** Blowing snow consumes it; nothing
  writes to it. Its one new output is a CPU-windowed copy of the ground slab for shaders.
- **Reuse over new tiers.** Drift is a separate particle pool with riser-shaped emission, not a
  `SSPrecipTier`; granular runoff is a re-feed of `shedRegion`, not a second shed path.
- **Ground exact, air approximate.** The field, regimes and lift are deterministic. The
  display-density texture (`SSWhiteout`) is the one GPU-resident, approximate layer - relaxed
  toward CPU-authored targets, never read by any state-owning system, one-way:
  field -> targets -> air.
- **Transport is extracted.** Lift/creep/deposit/spill live in `SSGranularTransport` as a pure
  stepped subsystem; `SSSurfaceField` is storage, windows, passes and plumbing.
- **Fixed-step ticks.** The field tick and regime transitions step in exact quanta of shared
  time, so transport is frame-rate independent and identical across viewers.

## Module map

| File | Change | Responsibilities added |
|---|---|---|
| `ssgranular.h/cpp` | **new** | the transport subsystem: lift, creep, deposit, spill, hysteresis - `step(Field&, const Geometry&, const Params&, F32 dt)`; pure and testable, no singletons inside |
| `sswindflow.h/cpp` | modify | `sampleGround()`, the ground-window GPU texture + binding (with the lift x depth target packed into its spare channel) |
| `sssurfacefield.h/cpp` | modify | fixed-step `tick()` calling the transport, `depositAt()` (thin forwarder), `renderSnowPass()`, extended `Sample` |
| `ssatmomagic.h/cpp` | modify | `liftAt()`, `squallFactor()`, the regime machine (derived, hysteresis+dwell, bounded change signal), granular deposit routing in `processImpacts()` |
| `ssprecippreset.h/cpp` | modify | granular authoring fields, `isGranular()`, the Sand built-in |
| `ssprecipitation.h/cpp` | modify | `mDrift` pool, `updateDrift()`, `emitDrift()`, granular stream/drip look, near-camera ring (Low-tier screen-space fallback) |
| `sspreciprenderer.h/cpp` | modify | one extra source loop over the drift pool in `render()`'s batching |
| `sswhiteout.h/cpp` | **new** | the whiteout pass singleton: intensity state, display-density layer (advect + relax mini passes), half-res density + composite |
| `ssavatarwet.h/cpp` | modify | caking channel on the existing capsules |
| `shaders/class1/deferred/ssSurfaceSnowF.glsl` | **new** | snow surface: coverage, albedo lift, creep scroll, POM, sparkle |
| `shaders/class1/deferred/ssWhiteoutF.glsl` | **new** | density pass (half-res, band march x air texture), composite entry |
| `shaders/.../ssPrecipRainF.glsl` (and `ssPrecipLitF.glsl`) | modify | the dithered granular branch |
| `llviewershadermgr.cpp` | modify | `gSSSurfaceSnowProgram`, `gSSWhiteoutProgram` registration (wet-pass pattern) |
| `pipeline.cpp` | modify | snow pass call in the wet block; whiteout call after `doAtmospherics()` |
| `settings.xml` | modify | the `SSAtmoSnow*` / `SSAtmoWhiteout*` keys |
| `sssoundscape.cpp` | modify (phase 5) | snow surfaces at the `STEP_TERRAIN_*` resolution site (line ~1350); regime audio bed crossfade subscriber |
| `llviewermenu.cpp`, `ssfloatersim.cpp`, `ssatmomagic.cpp` (`drawInfo`) | modify | debug entries + stats lines |

## Data model

### `SSSurfaceField::Field` (per region)

Existing: `mZ`, `mWet`, `mSnow`, `mPuddle`, `mStore` (shed accumulator), `mAccum` (spawn
accumulator). Added:

- `std::vector<F32> mLift` - the per-cell lift figure (smoothstep of local ground wind against
  the threshold band, temperature-gated). Recomputed each tick; read by `sample()` and by drift
  spawning. Not uploaded to the field window - it is CPU-consumed only; the shaders that want a
  lift-like figure (whiteout corridor term) read the wind flowmap's ground window directly.
- Creep needs no storage: the advection exchange is computed cell-by-cell inside `tick()` into a
  scratch row (double-buffer one row, not the whole grid).

Window textures unchanged in phase 1 (RGBA32F `Z, wet, snow, puddle`; flow window
`slopeX, slopeY, slopeNorm*wet, spare`). Phase 5 (compaction) takes the flow window's spare
channel and, if it needs a second, adds one more field texture - the layout decision is deferred
with the phase, and `bindFlowForShader` is the only code that would change.

### `SSPrecipSim` particle pools

Existing: `mParticles` (tiered falling), `mRipples` (impacts/drips), `mStreams` (persistent
cascades). Added: `mDrift` - ground-emitted blowing snow and the near-camera ring, one vector,
its own count and cap, rendered through the same material batching (see renderer below). Drift
particles are ordinary `SSPrecipParticle`s; the integrate loop does not see them (they carry
their own simple motion: horizontal flow plus a decaying loft, integrated in `updateDrift`), so
tier counts, tier targets and tier culling radii are untouched.

### The wind flowmap ground window (`SSWindFlowMap`)

The flow tiles are CPU-resident post-readback. A camera-centred window over the bottom slab,
rebuilt on the same cadence as `SSSurfaceField::updateWindow()`:

- per cell: `(flow_x, flow_y, speed, exposure)` into an RGBA32F texture,
- plus the origin/cell/res uniforms `ssWindOrigin` (mirroring `ssFieldOrigin`'s layout so the two
  windows fetch identically),
- one allocation, `glTexSubImage2D` per update, evicted with the tiles.

This window is the shader-side "where is the wind faster than ambient" answer for both the
whiteout corridor term and the snow surface pass's creep scroll.

## Key APIs

```cpp
// ssgranular.h - the extracted transport (pure; no singletons, no settings lookups)
struct SSGranularParams
{
    F32 mLiftLo, mLiftHi;        // the m/s band
    F32 mLiftRate, mDepositRate, mCreepRate;
    F32 mDepositGap;             // deposit threshold = mLiftLo * mDepositGap (hysteresis, ~0.7)
    F32 mLiftTemp, mSnowDepth, mReposeRad;
    F32 mGust;                   // gust envelope, applied once per tick, never per cell
    const LLVector4* mFlow;      // n*n ground-flow grid (xyz wind, w exposure), pre-sampled by
                                 // the caller - regimes deliberately do NOT appear here: the
                                 // regime directs, the field decides
};

namespace SSGranular
{
    void step(SSSurfaceField::Field& fld, const SSSurfaceField::Geometry& geom,
              const SSGranularParams& p, F32 dt);
}

// sswindflow.h - the cheap field read and the shader window
LLVector3 sampleGround(const LLVector3& pos_agent) const;   // first air slab above pos.z, NO gust
bool bindGroundWindow(LLGLSLShader& shader, S32 channel);    // + ssWindOrigin uniform
                                                             // spare channel: lift x depth target

// ssatmomagic.h - the single lift authority and the regime machine
F32  liftAt(const LLVector3& pos_agent) const;   // 0-1: band x sampleGround x temp (rate and gust
                                                 // applied by the caller, once per call site)
bool granularWeather() const;
F32  squallFactor() const;                        // 0-1 derived, smoothed
void fillTransportParams(SSGranularParams& params) const;   // assembled once per tick
enum class ERegime { CALM, SALTATION, DRIFT, BLIZZARD, SQUALL };
ERegime regime() const;                           // derived; hysteresis + dwell; fixed-step transitions
typedef boost::signals2::signal<void(ERegime, ERegime)> RegimeSignal;  // bounded subscribers:
RegimeSignal& regimeSignal();                     // soundscape, floater stats, whiteout ramp

// sssurfacefield.h - the one write path, and the snow pass
void depositAt(const LLVector3& pos_agent, F32 depth);  // forwards to the transport's repose logic
void renderSnowPass();                                   // called inside renderWetPass()'s block
Sample sample(...) const;                                // Sample gains F32 mLift

// ssprecipitation.h - the drift pool
void updateDrift(F32 dt);                                // field walk + camera ring + integrate
void emitDrift(const LLVector3& ground, const LLVector3& flow, F32 lift, SSRandStream& rng);
const std::vector<SSPrecipParticle>& drift() const;
S32  driftCount() const;

// sswhiteout.h - new singleton; owns the system's only GPU-resident weather state
class SSWhiteout : public LLSingleton<SSWhiteout>
{
public:
    void idle(F32 dt);     // ramped intensity state (regime rates), window liveness
    void render();         // air-layer mini passes, half-res density pass, composite
    void clear();
    void releaseGL();
    F32  intensity() const;
private:
    LLRenderTarget mDensity;      // half-res
    LLRenderTarget mAir[2];       // ~1282 ping-pong, advected toward CPU-authored targets
    S32 mAirRead = 0;
    bool mDensityValid = false;
};

// ssprecippreset.h - authoring
F32 mSnowLiftRate   = 0.f;   // 0 = this type never blows
F32 mSnowDepositRate = 0.f;
F32 mSnowCreepRate  = 0.f;
F32 mSnowDriftAge   = 2.5f;
bool isGranular() const      // FLAKE and SOLID are granular; LIQUID and RISER are not
    { return mArchetype == SSPrecipArchetype::FLAKE || mArchetype == SSPrecipArchetype::SOLID; }
```

`SSAtmoMagic::liftAt` is deliberately the only place the band settings
(`SSAtmoSnowLiftLo/Hi`), the gust envelope, the temperature gate and the preset's rate meet. It
calls `sampleGround()` once - never `sample()`: the fbm gust evaluation is per-caller, and every
caller of `liftAt` applies `gustEnvelopeAt(sharedTime())` as a scalar multiplier itself.

## Frame flow

### CPU (idle, main thread, existing budgets)

1. `SSAtmoMagic::idle` - params, squall derivation (existing `refreshParams` grows the squall
   factor).
2. `SSWindFlowMap::update` - unchanged solve; ground window refresh appended (with the lift x
   depth target packed into the spare channel).
3. `SSSurfaceField::idle` -> `tick()` per region, in the existing cursor order, **stepped in
   exact `TICK_DT` quanta of shared time** (the `mTickAccum` accumulator formalised - transport
   must never vary with frame rate). Each region's tick calls `SSGranular::step()`, which runs
   four stages per cell: settle/melt (unchanged) -> lift (`mLift[i]`) -> creep (row-buffered
   downwind exchange, CFL-capped, spilling into `mStore[ui]` at edge cells) -> deposit from calm
   (the lee term, hysteresis gap below the lift threshold). `shedEdges()` then runs unchanged -
   for a granular preset its inflow is what creep spilled into `mStore`, for liquid the existing
   rain feed.
4. `SSAtmoMagic` regime evaluation on the same tick boundaries - derived from current params,
   hysteresis + dwell; a transition fires the bounded `regimeSignal` (soundscape bed crossfade,
   floater stats, whiteout ramp rates) and swaps the bundle scalars folded into
   `transportParams()`.
5. `SSPrecipSim::update` - falling tiers (unchanged) then `updateDrift(dt)`:
   - field walk: `SSSurfaceField` hands the sim lift cells around the camera (`forEachLiftCell`,
     a small iterator over `mLift` above a floor, camera-radius bounded), spawn weighted by lift
     x depth, deterministic from the shared-clock cell hash;
   - near-camera ring: a capped ring spawn when `liftAt(camera)` or `squallFactor()` is above
     zero, scaled by the regime's near-ring multiplier;
   - integrate the pool: flow advection plus decaying loft, ground clamp via
     `SSRainShadowMap::resolveColumn` on a slice, fade by `mSnowDriftAge`.
6. `SSAtmoMagic::processImpacts` - granular drips land: instead of ripples, `depositAt(land,
   clump_depth)`; the `from_runoff` flag already marks these.
7. `SSAvatarWet::idle` - soak (existing) plus caking gain/decay.
8. `SSWhiteout::idle` - ramped intensity state (the regime's rate limits applied to the demand
   curves; the pass takes the `min` of regime ramp and gust spike); no per-frame CPU field reads.

### GPU (render order)

1. Deferred gbuffer as today.
2. Wet pass block (pipeline.cpp:9761): `renderWetPass()` then `renderSnowPass()` - the snow pass
   reuses the same scratch targets sequentially and commits into the gbuffer attachments
   (albedo + specular via the existing commit shader; its attachment mask becomes a parameter).
   Ahead of all lighting, same reasoning as wet.
3. Lighting, SSAO, etc. - untouched.
4. Pool pass, `doAtmospherics()` site (pipeline.cpp:4453): immediately after
   `done_atmospherics`, `SSWhiteout::render()` - the air-layer mini passes (advect toward the
   uploaded targets, relax/decay), half-res density (depth, field window, wind ground window,
   band march with heightfield occlusion taps, multiplied by the air texture), then a composite
   lerp to the fog colour on the scene target. Before the weather block, so flakes and cascades
   stay in front of their own fog.
5. Weather block: `SSPrecipRenderer::render()` batches `mParticles`, `mRipples`, `mStreams` and
   now `mDrift` through the same material buckets; granular streams/drips take the dithered
   branch (below).

## Shader inventory

| Shader | Status | Content |
|---|---|---|
| `ssSurfaceSnowF.glsl` | new | includes `ssSurfaceFieldF.glsl`; coverage from the snow channel; albedo lift; roughness/gloss; creep scroll (wind ground window + time); POM (step count/range uniforms, taper by distance); sparkle hash; avatar containment via the capsule bind |
| `ssWhiteoutF.glsl` | new | density: band march (surface Z from the field window, occlusion taps), corridor term (wind ground window vs ambient), squall term (exposure march from `ssFieldAt`), depth falloff; composite entry lerps scene colour to fog colour |
| `ssPrecipRainF.glsl` / `ssPrecipLitF.glsl` | modify | granular branch: static Bayer/IGN screen hash vs alpha -> discard, opaque write with depth; distance crossfade back to blend; granular tint/scroll look |
| `ssSurfaceFieldF.glsl` | unchanged | `ssFieldAt` already returns everything the snow pass needs |
| `ssSurfaceNormalF.glsl` | unchanged | water waves stay water's; snow scroll lives in the snow pass |

New programs in `llviewershadermgr.cpp`, registered exactly like `gSSSurfaceWetProgram`
(deferred feature level, `bindDeferredShader`, texture channels via `mActiveTextureChannels`):
`gSSSurfaceSnowProgram`, `gSSWhiteoutProgram` (density) and `gSSWhiteoutCompositeProgram` (or a
second entry in the same program behind a uniform, if the two fits one file cleanly - decide at
implementation; two entries is the safer default).

## Settings (new keys, house style)

| Key | Type/default | Notes |
|---|---|---|
| `SSAtmoSnowQuality` | S32, 2 | 0 off - 4 ultra; drives the section 9 table |
| `SSAtmoSnowLiftLo` / `SSAtmoSnowLiftHi` | F32, 3.5 / 8.0 | the m/s band |
| `SSAtmoSnowDriftBudget` | F32, 0.15 | share of `SSAtmoParticleBudget` |
| `SSAtmoSnowCreep` | F32, 1.0 | creep advection rate multiplier |
| `SSAtmoSnowCascadeDither` | F32, 0.8 | 0 blend - 1 full stipple |
| `SSAtmoSnowSurfaceStrength` / `SSAtmoSnowDepthFull` / `SSAtmoSnowSparkle` | F32 | surface pass dials |
| `SSAtmoSnowPomSteps` / `SSAtmoSnowPomRange` | S32/F32, 16 / 32 | POM ceiling and near range |
| `SSAtmoWhiteoutStrength` / `Band` / `Range` / `Corridor` / `Scale` | F32 | band metres, range metres, corridor ratio, 0.5/1.0 res |
| `SSAtmoLodDrift` | F32, 1.0 | beside `SSAtmoLodDrops` |
| `SSAtmoSnowDebug` | S32, 0 | Debug floater styles (field overlay, lift, whiteout density) |

Reused unchanged: `SSAtmoRunoff`, `SSAtmoRunoffScale`, `SSAtmoRunoffRadius`,
`SSAtmoParticleBudget`, `SSAtmoDensity`, the `SSAtmoWindFlow*` family.

## Phase -> touch list

| Phase | Files |
|---|---|
| 1. Erosion + drift + fixed step | `ssgranular.*` (new: lift + settle handoff), `sssurfacefield` (fixed-step tick, storage), `sswindflow` (`sampleGround`), `ssatmomagic` (`liftAt`), `ssprecipitation` (`updateDrift`/`emitDrift`/`mDrift`), `sspreciprenderer` (one loop), `ssprecippreset` (fields), `settings.xml` |
| 2. Redeposit + regimes | `ssgranular` (deposit term, hysteresis), `ssatmomagic` (regime machine + `regimeSignal` + bundle scalars), `sssoundscape` (bed crossfade subscriber) |
| 3. Snow surface | `ssSurfaceSnowF.glsl`, `llviewershadermgr`, `sssurfacefield` (`renderSnowPass`), `pipeline.cpp` (one call) |
| 4. Whiteout + air layer | `sswhiteout.*` (new: density + air ping-pong), `ssWhiteoutF.glsl`, `sswindflow` (ground window + target channel), `llviewershadermgr`, `pipeline.cpp` (one call), `ssatmomagic` (`squallFactor`, ramp rates), `ssatmoenvweatherstate` (squall derivation + forecast text) |
| 5. Caking, footsteps, compaction | `ssavatarwet`, `sssoundscape.cpp:~1350` (+ `SSStepSurface` enums in `ssprecippreset.h`), `ssgranular` (compaction channel) |
| 6. Granular runoff | `ssgranular` (creep + spill), `sssurfacefield` (shed re-feed), `ssprecipitation` (granular stream/drip look), `ssPrecipRainF.glsl`/`ssPrecipLitF.glsl` (dither branch), `ssatmomagic` (`processImpacts` deposit routing), Sand built-in preset |

The design rationale for the extraction and the rejected alternatives (in-place monolith,
GPU-resident field, top-level fixed-step core) is in `doc/atmo_magic_snow_review.md`.

## Guards and known sharp edges

- **The Params struct is the seam.** `SSGranularParams` stays a plain aggregate of floats - no
  LLSD, no settings lookups, no singletons inside `SSGranular::step()`. The moment the transport
  reaches out for the world, the extraction has collapsed back into the monolith it exists to
  avoid.
- **One-way: field -> targets -> air.** The air texture is never read by any state-owning
  system; drift spawning reads `mLift`, never density. Enforced by convention and written here
  so review has something to point at.
- **The event seam stays bounded.** `regimeSignal` subscribers: soundscape, floater stats,
  whiteout ramp. A second event type gets promoted to a real pump consciously, not by accretion.
- **Fixed-step changes existing feel.** Rain/puddle dry-down timing shifts slightly when the
  accumulator locks to shared-time quanta. It is a change toward determinism, and it gets its own
  test-plan line rather than riding along silently.
- **`depositAt` is a forwarder, not a bypass.** The transport owns the repose cap; a debug-only
  assert on repose compliance catches any future caller writing `mSnow` directly and breaking
  the ledger.
- **`mLift` is per-tick state, not per-frame.** Consumers (drift spawn, near ring) read the last
  tick's figure; the tick interval (sub-second) is far below the visibility threshold for a
  quantity that ramps over seconds.
- **`depositAt` overflow is discarded, not re-routed.** Re-routing would need edge-cell store
  access from the impact path; the repose cap makes the loss invisible (a clump landing on a full
  pile barely changes it). Revisit only if eave piles ever visibly under-fill.
- **Drift never touches tier machinery.** `mTierCount`/`mTierTarget`/`tierBands` stay the falling
  tiers' alone; the drift pool has its own cap (`SSAtmoSnowDriftBudget`) and its own cull radius,
  so a blizzard cannot starve falling snow of budget or vice versa.
- **The ground window follows the flowmap's own validity.** While a tile rebuilds, the window
  keeps the last committed cells (they are CPU-resident until replaced) - no flicker, and the
  whiteout pass gates on window age like the surface pass gates on `mWindowValid`.
- **The dither branch is keyed on the particle's material flag, not the preset**, so a granular
  preset's ripples (if any) and the near-camera ring can choose per-particle.
- **`sampleGround()` returns the solved field, gusts excluded** - every consumer applies the
  scalar gust envelope itself, once per call site, never per cell. This is the rule that keeps
  the erosion tick and the spawn walk linear.
- **The Low-tier near-layer fallback is a documented lie**: below Medium, the near ring may
  degrade to a screen-space streak sheet. It never becomes the default path.
