# Atmo Magic: surface weather v2 — the synthesis plan

Status: **design plan** (2026-09-07, on `feature-atmo-magic`). This is the plan the
surface-material design competition converged on — see
`doc/atmo_magic_surface_competition.md` for the 14 competing designs, the three judges'
verdicts and the scoreboard. Nothing in this plan has been built; every phase lands
behind its own debug gate with the shipping path pristine until that phase's acceptance
criteria are met.

The one-sentence architecture: **the CPU ledger stays the only ground truth, and five
independent layers attach to it** — a resampling tick, a shallow-water flow core, an
emergent-repose granular v2, a per-object identity/record layer, and a near-camera hero
droplet detail layer — while the three proven screen-space passes remain the weather
application site throughout v2.

**Rebaseline (2026-09-07, post-merge).** Three merges landed after this plan was
written and they move its ground:

- **Roof runoff** (`f5d5e4460b`) materialised the DRAINAGE_NETWORK far beyond what P2
  planned: per-cell catchment (m²) routed down D8 with an eave rule (drop steeper than
  a roof pitch, ≥ 0.75 m, terminates flow into the air), connected lip **Runs** with
  summed catchment and stable keys, per-run reservoirs, catchment-weighted curtain
  streams and drips (`SSSurfaceField::Runoff`, `shedRegion`). **P2 is absorbed** (§3);
  **P4 is re-scoped** to its true remainder — the wall face film.
- **Avatar stamping fix** (`688130561b`) committed the interim capsule gate into
  `ssSurfaceNormalF` — that gate is now the parity baseline P6 must beat, not a
  working-tree patch.
- **World-field capture** gained adaptive quadrant refinement and Z bisection with
  capture worklists — structure is finer than the plan's flat-band assumption, which
  P4's wall ledger may key off (per-band lip/lip Z at refined precision) instead of
  raw bands.

## 1. Constitution (fixed by the competition, non-negotiable)

1. **CPU is the only truth, fixed-step.** No GPU-resident authoritative state. This is
   what funds the LOCKSTEP twin tests, texel-exact goldens and the no-`rand()` discipline;
   it is nearly free to keep and a sim rewrite to restore (Judge C's "regret decision").
2. **One write path per ledger.** New subsystems become write-path peers of
   `depositAt`/the settle pass, never bypasses.
3. **Off = byte-identical.** Every phase gates on an `SSAtmo*` setting; off means the
   existing window path runs untouched. No phase deletes a proven pass under its own
   gate (the D14 cliff lesson) — retirements happen only in a later phase, after parity
   is proven.
4. **Phase transitions never interpolate** (D11's rule, adopted fleet-wide). Wet,
   puddle, stain, age, snow depth advance by the analytic predictor; ice crossings,
   pool membership, repose/spill events, scorch holds and look-Mix promotions always
   full-step.
5. **Determinism.** Integer hashes only, total order on any worklist, frame-rate
   independence via the shared-time quantum clock.
6. **The invalidation path is the budget.** Every phase is costed at its worst case
   (teleport storm, rain onset, 40-avatar churn, dense forest), not its steady state
   (Judge A's "sunny day" trap).

## 2. Layer map (v2 end state)

```
SSWorldField  (0.25 m column tiles, bands, air labels, drainage — EXISTS, unchanged)
      |
SSSurfaceField::Field  (the ledger — EXISTS; gains stamps/activity in P1)
      |
  +---+------------+------------------+---------------------+
  |                |                  |                     |
Worklist +      Flow core         Granular v2          Object records
staleness       (P2-P4)           (P5)                 (P6)
(P1)            pipes+wall film   cohesion/repose      per-draw id
      |                |                  |                     |
      +----------------+--------+---------+---------------------+
                                |
                 Three screen passes (wet / normal / albedo)
                 + hero droplet composite (P7)
                                |
                             gbuffer
```

Height fog keeps its own `ssFieldFetch` (it answers for the column/air, a legitimately
field-shaped question) and is untouched throughout.

## 3. Phases

### P0 — Baseline instrumentation (prerequisite, small)

Capture the numbers every later phase is judged against.

- Block timers around the tick, window pack and each pass (most exist; add per-phase
  sub-zones). Record steady-rain and storm tick cost today.
- Freeze the harness world scenes the competition assumed: level pad, graded slope at
  repose, a wall with an eave, an overhang, a parking vehicle, two avatars — **plus
  the three stress scenes the guardrails demand (Judge C, R2): a dense forest parcel
  (alpha overdraw), a 40-avatar churn scene, and an 8-region teleport storm path.**
- Debug views for staleness, flow, film depth exist from day one (extend the existing
  debug-view registry).

*Acceptance: baseline numbers recorded in the harness; no behaviour change. Every
later phase's acceptance adds three stress gates against these baselines: storm-tick
≤ 2× steady-tick; window staleness ≤ 8 quanta during a storm; forest frame-time delta
≤ +0.1 ms vs P0.*

### P1 — Amortized tick (D11)

The full-grid 0.25 s tick becomes an importance-sampled event. This phase changes no
visuals by design; it builds the scheduling spine everything else runs on.

- **State:** per cell `mStamp` (U32 shared-time quantum last fully stepped), `mAct`
  (F32 activity score), one tolerance-class byte. Added to `SSSurfaceField::Field`
  (`sssurfacefield.h:204-247`) — the vectors stay the checkpoint; nothing is removed.
- **Worklist:** each tick, K ≈ 4096 cells win a deterministic hash-threshold draw on
  activity (wet/puddle gain, exposure change, wind events, phase-danger ±1 °C of
  0 °C forced full-step) and get the full `stepCell` + transport step; every other
  cell is carried by the closed-form predictor the tick already computes
  (`1 - exp(-rate·dt)`). Total order on (region, cell, quantum), hash tiebreaks.
  *Determinism note (Judge B): the draw must be pure in (region, cell, quantum) —
  camera proximity is client-local and is excluded from the set-selecting hash; it
  may bias step *priority* only where that cannot change the selected set. The
  equality claim is bitwise-given-identical-inputs; cross-camera behaviour is
  convergence, tested by T4.*
- **Staleness contract (Judge B fix):** `ssFieldAgeMap` RGBA8 carries **`mStamp`,
  rate and class — never the live staleness value** (that would change every tick and
  degenerate the touched-rows pack into a full repack). The shader derives staleness
  as `(currentQuantum − stamp)·TICK_INTERVAL` from one uniform. Rate is stored in a
  twinned fixed-point encoding so the shader's `rate·staleness` matches the CPU
  predictor exactly at 8 bits (`twin_surface_agepack`). Saturating stamps
  (255 quanta ≈ 64 s) force a full step.
- **CPU readers (Judge A gap):** `sample()` (`ssavatarwet.cpp:66`) and
  `forEachLiftCell` (`ssprecipitation.cpp:1109`) read raw vectors — each read applies
  the same closed-form advance at read time, or force-promotes its cells that tick.
  Also: the settle loop's peak scan feeds `advanceMix` — peaks must stay tick-scoped
  via an O(1) max-scan over the full arrays, or look crossfades stall under partial
  stepping.
- **Ramp promotion (Judge C, R6):** preset changes stamp whole regions *and* a
  d(intensity)/dt threshold stamps on continuous ramps — otherwise a rising-storm
  front arrives one quantum late everywhere.
- **Window pack:** only touched rows repacked; persistent-mapped PBO sub-range uploads
  replace the four full-texture `glTexSubImage2D` calls (`sssurfacefield.cpp:1909-1931`).
  Origin moves upload whole — dirty-row skipping applies only when the camera-anchored
  origin is unchanged, else it silently degrades (build-time risk R3 in §8).
- **Forced promotions:** `markDirty`, external writes (`depositAt`, `vaporise`),
  preset changes and gust fronts stamp whole regions (the existing `ran > 1` replay
  path already models this).
- **Gate:** `SSAtmoSurfaceResample` (0 = pristine full tick).
- **Tests:** predictor/CPU twin equality grid test (twin harness discipline); bitwise
  determinism replay; burst-rain resync; puddle-edge cells at 2× rate.
- **Budget:** >10× tick cost cut at steady rain (target on `FTM_SS_SURFACE_TICK`);
  predictor cost is O(1) per stale cell.

### P2 — Flow core, static tier (ABSORBED by the runoff merge, 2026-09-07)

The runoff work built past this phase's goal: `SSWorldField::buildDrainage` now fills
depressions, routes D8 with an eave rule, accumulates catchment, and
`SSSurfaceField::Runoff` materialises it over a finer grid with connected lip runs,
per-run reservoirs and catchment-weighted shedding. Nothing needs claiming or solving.

**Residue (reviewed 2026-09-07, Judge A — one was wrong, now a finding):**
- Pool membership: **verified** — the puddle fill tests `geom.mPool[i]` for every
  solid, non-water cell region-wide (`sssurfacefield.cpp:1254`); camera radius gates
  only stream/drip emission. Gate dependency named: the drainage mask lands only when
  the world field is the grid source (`SSWorldFieldSurfaceTop`, default 1; the
  local-dips check at `sssurfacefield.cpp:232-255` is the off-path). The P2 acceptance
  scene must run with the world-field source.
- Sheet-flow direction: **finding, not a confirm** — the flow window is filled from
  raw central-difference cell slope (`sssurfacefield.cpp:1819-1822` from
  `buildGeometry:228-229`); `buildRunoff`'s D8 directions never reach any texture.
  Water does **not** visibly run toward the outlet today. Fix rides P3's flow field
  (`u` replaces the scroll source); pulled forward into P2 only if the acceptance
  scene must show outlet-ward flow before P3.
- Reservoir persistence across retraces: verified in code (stable run keys, orphan
  mass redistributed by catchment, `sssurfacefield.cpp:503-517, 637-682`) — but a
  retrace that *changes a run's biggest feeder* re-keys and drops mass mid-storm;
  test owns this (T6).
- Debug view: `renderRunoffDebug` at `sssurfacefield.cpp:3084`.

### P3 — Flow core, dynamic tier (virtual pipes)

- **New files** `ssflowfield.h/.cpp`, core laws in `ssflowcore.h` (LOCKSTEP, twinned
  to GLSL per the `twin_surface_glsl.cpp` discipline).
- **State:** a 128² × 0.25 m camera window (32 m) parallel to the field window:
  film depth `h`, four pipe fluxes, suspended sediment `C`, bed erosion offset, ice
  fraction. `mPuddle` becomes the render view of `h`.
- **Sim:** three 1/12 s substeps per 0.25 s shared-time tick on the CPU (16 cores,
  row-striped, gather-only reads); pipe damping factor absorbs the stiffness term so
  0.083 s substeps hold with 2× CFL margin at 0.25 m cells; splashes never enter the
  heightfield (they are ring events). Order-independent inflow accumulator — the exact
  discipline `mInflow` already uses for creep (`sssurfacefield.h:199`).
- **Sleep/wake:** cells parked at the flood's `spill − z` equilibrium cost nothing;
  wake on rain, ice transition, avatar splash, edit. Most of the world never steps.
- **Couplings:** ice scales pipe area by `(1 − ice)`; freeze/thaw events re-run the
  Barnes flood (event-driven, not per tick) so a frozen pool dams and overflows at a
  new spill; melt credits `h`; Mei capacity `Cmax = Kc·|u|·sinα` drives
  erosion/deposition between `C` and the deposit channel.
- **Render:** rivulets are `|u|`-high / `h`-low cells modulating the sheet wave
  texture; `ssWetFlowMinWet` becomes the physical film threshold.
- **Uploads:** persistent-mapped 4-deep PBO ring with `glFenceSync` per slot (GL 4.6,
  `glBufferStorage` persistent+coherent).
- **Gate:** `SSAtmoFlowSim = 2`.
- **Budget:** 16 k cells × 3 substeps ≈ 2 MFLOP/tick — sub-millisecond, single core;
  escalation row-parallel across 16 cores.
- **Tests:** pipe-step twinned in the unit harness; determinism replay; wall-run mass
  conservation vs ground credit (P4).

### P4 — Flow core, wall face film (verticals) — re-scoped post-merge

The runoff merge took the eave half of this phase: lip runs, per-run reservoirs,
curtain streams and catchment-weighted drips with landing resolves exist
(`shedRegion`). What remains is the **wall face itself** — the clinging film that rain
drives onto vertical faces and that curtain streams and eave drips land on, which the
heightfield cannot represent and nothing yet simulates.

- **Interface (new, small):** `shedRegion`'s landing resolve already classifies each
  stream/drip landing (`land`, `on_water`) at `sssurfacefield.cpp:989-995` and
  `:1034-1040`; add a wall-hit branch — a landing whose first surface is a steep face
  feeds the wall ledger at that entry instead of only splashing. Curtain stream drive
  and drip pick stay catchment-weighted; they simply gain a downstream sink.
  *Classifier (Judge B): `resolveColumn` returns no normal and cannot see vertical
  faces at vertical rain — the branch classifies with a worldfield column test /
  span-store lateral trace (`SSWorldField::traceSolid`) at the landing point, never a
  screen-space derivative; the splash-vs-wall mass split is conservation-accounted
  (T8).*
- **Wall ledger:** sparse per-region map keyed `(azimuth 0..15, v-band 0.25 m,
  along 0.25 m)` — the 16-azimuth quantisation *is* `SSSurfaceDrop::DRIP_AZIMUTHS`
  (`sssurfacedropcore.h`), so sim and shader share one bit-stable frame. Optionally
  key lip heights off the world field's refined spans (quadrant refinement / Z
  bisection) instead of raw 4 m bands.
- **Film model:** per entry `h_wall` — driven up by the wall-hit branch above plus
  wind-driven rain deposition on the face; decays by Nusselt laminar drainage
  `q = ρg·h³/3μ` down-v, wind diffuses along-u, porosity absorbs. Below breakup
  thickness `h_crit ≈ 0.15 mm·(1 − roughness)` the contact line fails: the entry
  converts its depth into run seeds in the existing 0.06 × 0.30 m drip cells, each
  carrying mass down at accelerating speed and leaving a trail; runouts at the wall
  base credit the macro field's `h` — closing the loop with the ground ledger.
- **Render:** the drip cap/trail code (item 2) keeps its shape but the wall drip's
  phase comes from the sim's accumulated run distance instead of `ssTime` — the shed
  side's drips are already state-driven (reservoir-fed, catchment-weighted); this
  makes the *face runlets* state-driven too. The Heartfelt stutter becomes a property
  of the run, not a wall clock.
- **Gate:** `SSAtmoWallFilm` (separate from `SSAtmoFlowSim`, which is now the ground
  sim's gate).
- **Honest scope:** 0.25 m cells cannot resolve centimetre rivulet fingers — streak
  breakup remains a state-modulated lattice; the sim drives its amplitude and phase.

### P5 — Granular v2 + footprints (D10 scoped, CPU)

MPM on GPU is rejected for v2 (atomics concede bitwise cross-viewer equality; Judge C
scores it 4 on perf for calm-weather overkill). What survives is the constitutive
upgrade, on CPU, through the single write path.

- **Emergent repose:** Drucker-Prager-style yield with friction angle per material
  replaces the `roomAt` heuristic (`ssgranular.cpp:68-102`); wet-modulated cohesion
  `c·(1 − wet)` — cohesion raises the angle damp, buoyancy lowers it saturated, so
  drying then avalanches. Melt mass converts into the flow ledger's `h` (P3 coupling).
- **Footprint masks:** movers and avatars write per-frame small carve/compression
  masks into a window texture; a stepped pass applies them as depth displacement and
  grid impulse **through `depositAt`** — the single write path. Footprints, ploughing
  and compression follow the actual geometry that made them.
- **Wind entrainment stays in `liftAt`** — the one lift authority is not re-derived.
- **Gate:** `SSAtmoGranularV2` (0 = current heuristic repose).
- **Budget:** CPU tick already amortises (P1 worklist); masks are one 256² texture
  pass per frame near the camera.

### P6 — Object identity + records (D6 phase 1, D1's PoC grown up)

This is the phase that deletes the capsule hack, in both passes, without any
per-pixel sim.

- **Id attachment:** one `R16UI` `ssIdRect` (0 = none), written behind a `HAS_SS_ID`
  define by weather-relevant writers — **avatarF and the simple pools first** (D1's
  one-day PoC scope), expanding to material/rigged variants. Cleared per frame; a
  debug view audits coverage (a missed writer reads stale texels — this is the known
  failure mode, tested not hoped away). *Exclusion semantics (Judge C, R3): writers
  are an allow-list of OPAQUE pools only — alpha/blended writers would need integer
  blending (a GL error) and R16UI MSAA resolve is undefined, so the attachment is
  allocated resolve-off and impostor/flexi/foliage pixels keep id 0, falling through
  to the field/column answer; the class split impostors need (so a distant impostor
  card doesn't re-inherit ground weather) is P6-expansion scope, named as a known
  limitation until then.*
- **Record table:** SSBO, 4096 rows × 32 B `{wet film, ice, deposit, thermal,
  exposure, class, geometry serial, soak}` — **CPU-owned**, stepped per tick by the
  existing `SSAvatarWet` soak/exposure integration (that part is real simulation and
  survives); refcounted ids with a serial so recycled ids never bleed stale soak.
- **Pass change:** the capsule loops (`ssSurfaceWetF.glsl:138-226`, the interim
  `ssAvatarContain` in `ssSurfaceNormalF.glsl`) are replaced by one id fetch + record
  read. The wet pass's `ground_share` knee ramp keys off the record's height-band
  answer exactly as before.
- **Movers:** parked vehicles gain a record (field answers via the column test when
  parked; the record carries drying while driving — the "tarp into a barn" gap is
  documented as a known limitation, not hidden).
- **Gate:** `SSAtmoObjectWeather` (0 = capsule path untouched).
- **Retirement (after parity):** `SSAvatarWet::bindForShader` calls and the capsule
  upload die; the singleton shrinks to the CPU soak integrator feeding records.
- **Budget:** R16 = 2 B/px ≈ 4 MB @1080p, ~0.3 ms write bandwidth; record step is
  trivial (≤ a few thousand rows).

### P6.5 — Frost physics + crack relief (Judge B's uncovered gap; small, parallel-safe)

Decided 2026-09-07: **scheduled as a small standalone item, not v3** — ice is a
first-class required feature of the brief, the state channels (`mIce`, `mFrost`) and a
crack lattice already exist, and the gap is fidelity, not machinery. Independent of
P3-P6; lands any time after P1.

- **State (CPU, deterministic):** crack damage as accumulated freeze-thaw cycling —
  per cell a damage scalar that rises when a wet cell's ice fraction crosses 0 in
  either direction, saturating over several cycles; frost coverage as an
  exposure × humidity × sub-zero duration law stepping in `SSSurfaceState::stepCell`
  (the channels exist; the laws sharpen). Damage persists into thaw (scars), so a
  repeatedly-frozen puddle keeps its crack pattern.
- **Render (both passes — Judge C, R7):** the axis-aligned 0.35 m crack lattice
  becomes a Voronoi-edge hash lattice seeded per cell, as a **shared include consumed
  by the normal pass's relief block AND the albedo pass's crack-line block** (both
  carry the lattice today at the same pitch; upgrading one alone desyncs relief from
  lines). Crack line density and width scale with the damage scalar; frost renders as
  a micron-scale grain tint on the existing deposit grain path. Twin test asserts the
  two passes' lattice functions agree.
- **Gate:** `SSAtmoIceFrostV2` (0 = current crack lattice verbatim).
- **Tests:** damage accumulation twin test; scar persistence across melt/refreeze;
  determinism replay.
- **Budget:** a few FLOP per full-stepped cell inside the P1 worklist; one extra hash
  tap in each pass's ice/crack block (shared function).

### P7 — Hero droplets (D8, the detail layer)

The only GPU sim in v2, and it is cosmetic by construction: the macro stays CPU
truth; heroes are sub-second-lived bodies checked out of the ledger.

- **Spawn:** the flow core's film-thinning (P4) is the spawn condition — a hashed
  roll launches a run where `mFilm > h_c`; heroes are where those probabilities cash
  out as visible bodies near the camera (≤16 m, N ≈ 512).
- **Sim:** compute pass at shared-clock quanta; states CLING/RUN/FREE; gbuffer
  re-projection adhesion (depth + gbuffer normal, never the derivative normal);
  merge on proximity, breakoff when mass exceeds the cell's capacity; FREE heroes
  become impact-ring events through the existing `noteImpact` path.
- **Render:** half-res splat → bilateral smoothing → normals-from-gradient →
  composite in the normal pass after item 1; depth-ε match, no position buffer needed.
- **Ledger handoff (Judge B/C determinism fix):** a dying hero's mass return is
  **debit-at-spawn, ledger-bookkept** — the spawn walk checks mass out of `mFilm`
  deterministically, and the refund on death is a tick-aligned, cell-stated function
  of ledger state (never the GPU hero's exact merge-order mass), routed through
  `depositAt`. With `SSAtmoHeroDrops` on, hero *placement* is conceded cosmetic
  non-bitwise; the ledger itself stays bitwise (T13 proves it).
- **Gate:** `SSAtmoHeroDrops` (0 = lattice items 1/2 verbatim, the shipping look).
- **Budget:** ≈ 1 ms GPU total @1080p (0.4 sim + 0.6 splat/smooth/composite, half-res).
- **Known failures (accepted, bounded):** silhouette/disocclusion pops; the 16 m
  content cliff; thin-wall merge artifacts; hero placement not bitwise-identical
  across clients (macro converges; heroes don't — conceded because they are cosmetic).

### P8 — Coverage beyond the window (D12, conditional — not scheduled)

Only if the window proves structurally insufficient: per-region bake set (512² @ 0.5 m +
wall-strip atlas) produced by the 16-core job graph, streamed via the persistent ring
at ≤ 4 MB/s, movers keeping per-object 64² overlays. The competition scored it 8 on
perf precisely because its worst case is a 2 s visibility artifact, never dropped
frames. Deferred until P1-P6 make the cost real and measurable.

**Promotion tripwires (decided 2026-09-07).** P8 is promoted from conditional to
scheduled only by measurement, and only after the cheaper mitigations fail:

1. **Coverage tripwire** — a debug counter samples, once per frame, the share of
   surface pixels whose field fetch returned "outside the window" while the nearest
   in-window border cell is meaningfully wet (the visible dry/wet discontinuity
   condition, not just any miss). Promotion when this exceeds **1% of surface pixels
   sustained over a 60 s window of normal play** (not synthetic stress) — the fraction
   a player can see as a wet/dry seam at the window border.
2. **Warmup tripwire** — post-teleport parity time: seconds from teleport until the
   window around the arrival point is fully stepped and visually consistent.
   Promotion when this exceeds **3 s** measured on the harness teleport scene.

Mitigation-first rule: before scheduling P8, try (a) widening the window at reduced
cell fidelity (the P1 predictor makes coarse far cells cheap), (b) predictor-driven
far-field answers past the border (the ledger knows regions it no longer streams).
P8 is the answer only when both distort the near field or still leave seams.

## 4. Deletion plan (what v2 removes)

Retirement timing rule (Judge B): every row is **added under its gate at the named
phase and deleted only when that gate flips to default-on** (or at a named later
retirement phase) — never deleted at the phase that introduces its replacement, or
the off path breaks (the D14 cliff).

| Thing | Added under gate | Deleted at |
|---|---|---|
| Capsule geometry in wet pass + `mShaded` upload | (shipping today) | `SSAtmoObjectWeather` default-on (P6 parity) |
| Interim normal-pass capsule gate (`ssAvatarContain`) | (shipping today, `688130561b`) | same |
| Slope-derived scroll direction in sheet term | (shipping today) | `SSAtmoFlowSim` default-on (P3 parity) |
| Painted granular shed cascade (`mStore` cursor path — note: the *liquid* shed side is already replaced by the runoff merge) | (shipping today) | retirement review after P3+P4 parity |
| `roomAt` repose heuristic | `SSAtmoGranularV2` | that gate's default-on |
| Four full-window `glTexSubImage2D` per tick | (shipping today) | `SSAtmoSurfaceResample` default-on (P1 parity) |

`ssavatarwet.h`'s CPU soak/exposure state machine survives to end-of-v2 as the record
stepper; only its screen-space geometry dies. The avatar-soak debug view migrates onto
records at P6 (named so nothing strands).

## 5. Guardrails (the competition's worst-case lessons)

- **Invalidation storms:** teleports and preset changes stamp whole regions — the
  predictor absorbs the span, the worklist drains at bounded K, the window refills
  from the static tier. Guard: per-tick full-step ceiling, overflow defers one quantum.
- **Dense forest:** v2 adds no per-fragment cost to material writers and no per-pixel
  attachment beyond P6's R16 write (2 B/px, gated pools only). Heroes render half-res.
  The forest benchmark runs at every phase gate.
- **Camera cuts / context loss:** cut detector resets rings and extrapolation
  uniforms one frame; heroes die on disocclusion (mass returns); no history buffer
  exists anywhere else to lose — that is a deliberate property of this architecture.
- **Sub-tick transients:** 0.25 s ticks quantise fast run motion; interpolation of
  run *phase* is visual-only and never feeds back into state (determinism preserved).

## 6. Budget summary (RTX-class, 1080p, worst cases)

| Phase | GPU | CPU | VRAM |
|---|---|---|---|
| P1 resample | — | tick −10× (vs post-runoff tick) | +1 RGBA8 window (~0.26 MB) |
| P2 absorbed | — | verify only | 0 |
| P3 pipes | — | < 1 ms/tick, sleeping world free | +1 window set (~1 MB) |
| P4 wall film | — | < 0.5 ms/tick + shed-landing classification (shed timer) | sparse wall ledger < 4 MB |
| P5 granular v2 | — | inside P1 worklist | +1 mask texture (256²) |
| P6 records | ~0.3 ms write | trivial | +4 MB @1080p (**16.6 MB @4K** — footnoted per Judge B) + 128 KB SSBO |
| P7 heroes | ≈ 1 ms | — | ~2 MB targets |
| Total v2 | ≈ 1.3 ms @1080p | tick cheaper than today | ≈ 8 MB @1080p (~25 MB @4K) |

Unpriced-but-named (Judge B): P3's event-driven Barnes reflood re-runs per freeze/thaw
event — a hard-freeze front re-floods the grid at transition frames; priced against
the storm gates. `shedRegion`'s per-landing resolves already live on
`FTM_SS_SURFACE_SHED` and P4's branch adds to that timer, not the tick.

Compare the competition's rejected alternatives: D3's resident cascade (5 MB + GPU
truth), D2's visibility buffer (18.6 MB + every writer twice), D9's PBF (standing
3-5 ms SDF tax).

## 7. Decisions (resolved 2026-09-07)

1. **P6 first-landing scope: avatar + simple pools only** (D1's PoC scope), expanding
   to material/rigged variants after parity. Adopted.
2. **P5 footprints: CPU-stepped** through the worklist, masks applied as `depositAt`
   writes. Determinism preserved. Adopted.
3. **P8 promotion: dual measured tripwire** — (1) outside-window wet-pixel share
   > 1% sustained 60 s of normal play, (2) post-teleport parity > 3 s on the harness
   scene — and only after the mitigation-first rule (widen window, predictor far-field)
   fails. Recorded in §3 P8.
4. **Frost/ice cracks: scheduled as P6.5**, a small standalone item (state laws in
   `stepCell` + Voronoi crack lattice in the normal pass), parallel-safe after P1.
   Recorded in §3.

## 8. Review record (three judges, 2026-09-07)

Full fact-check / architecture / risk verdicts ran against the post-merge tree.
Scores: factual accuracy **8/10** (Judge A), architecture **7/10** (Judge B), plan
readiness **7/10** (Judge C). Their must-fixes are applied in place above, marked
`(Judge A/B/C)`; the material ones:

1. P2's sheet-direction "confirm" was **false** — flow window carries raw cell slope;
   D8 never reaches a texture. Now a finding; fix rides P3's `u`.
2. P1's staleness texture must carry **stamps, not live staleness** (else touched-row
   packing degenerates to full repack); camera proximity is out of the set-selection
   hash; `sample()`/`forEachLiftCell` and the `advanceMix` peak scan are named CPU
   readers that the worklist must not strand; rain *ramps* promote, not just presets.
3. P4's wall-hit classifier must be a worldfield column/span test — `resolveColumn`
   cannot see vertical faces; splash-vs-wall mass is conservation-accounted (T8).
4. P6's R16UI is **opaque-pool allow-list only** (no integer blending, MSAA resolve
   undefined); impostor/flexi/foliage stay id 0 with the class split named as
   expansion scope.
5. P7's hero→ledger refund is debit-at-spawn, tick-aligned bookkeeping — the ledger
   stays bitwise even with heroes on (T13).
6. Cross-gate sinks defined where a phase's consumer gate is off (melt→`mPuddle`
   fallback with `SSAtmoFlowSim` off; `SSAtmoFlowSim`'s static tier now belongs to
   the shipping runoff path — its ladder reduces to off/dynamic).
7. Deletion plan rewritten as add-under-gate / delete-at-default-flip; P0 gained the
   forest/churn/storm scenes with numeric stress gates; P6.5 extended to **both**
   passes; budget table corrected (P2 absorbed, 4K footnotes, shed-timer naming).

Named test list (owners per phase): T1 predictor/CPU divergence at phase transitions,
T2 look-Mix determinism under partial stepping, T3 ramp-onset front latency,
T4 cross-camera border convergence, T5 staleness saturation, T6 reservoir persistence
across feeder-changing retraces, T7 flow-window re-entry without mass spike,
T8 wall-hit mass split conservation, T9 freeze-thaw crossing not missed between
resamples, T10 Voronoi lattice parity across passes, T11 id coverage audit automation
(alpha occlusion, impostor, MSAA), T12 record id recycle bleed, T13 hero-handoff
ledger equality.

## 9. Implementation outline (first slices — P0, P1, P3, P4)

Insertion points verified against the current tree; P5/P6/P6.5/P7/P8 stay one-line
placeholders until their gates open.

**P0** (no behaviour change): sub-zone timers around the settle loop
(`tick()`, `sssurfacefield.cpp:1164-1298`), transport call (`:1312-1326`) and the
window pack loop (`updateWindow()`, `:1793-1829`) — `FTM_SS_SURFACE_TICK` already
exists at `:127`; extend the `SSAtmoSurfaceDebug` switch (`:2035`, pattern
`renderRunoffDebug()` `:3084`) with staleness/flow/film views; freeze the six
harness scenes plus the three stress scenes.

**P1**: add `mStamp`/`mAct`/`mClass` to `Field` (`sssurfacefield.h:204-247`,
allocated in `fieldFor()` `sssurfacefield.cpp:1052-1072`); restructure the per-cell
loop into K-winners full path (`stepCell` call site `:1279`, `washRemoval` `:1285`)
vs closed-form carry (reuse the `1 - exp(-rate·dt)` blend at `:1103`); force-promote
in `depositAt` (`:1630`), `vaporise` (`:1505`), the `REBUILD_DZ` reset (`:1178-1183`)
and preset changes in `idle()` (`:1367`), plus `SSWorldField::markDirty` — which the
field does not have yet and P1 adds; fifth window allocated beside `mWindowCoverTex`
(`:1883-1891`), filled in the pack loop, fifth upload in the `glTexSubImage2D` block
(`:1909-1931`), `bindAgeForShader` cloned from `bindStateForShader` (`:1971-1982`),
bound in all three passes; the predictor advance lives in **one site** —
`ssFieldAt` in `ssSurfaceFieldF.glsl:126-208` — so wet/normal/albedo inherit it;
`SSAtmoSurfaceResample` uniform rides `bindLooksForShader` (`:1999-2047`).
Tests: `twin_surface_predictor` (predictor ≡ stepCell over the grid),
`twin_surface_resample` (bitwise replay across burst-vs-single quantum delivery —
the worklist hash includes the quantum index so replay selects identical cells),
`twin_surface_agepack` (RGBA8 stamp/rate/class round-trip), T1-T5.

**P3**: new `ssflowfield.h/.cpp` + LOCKSTEP `ssflowcore.h`; 128² window stepped from
`idle()` after the tick loop (`:1463-1493`) and before `shedEdges` (`:1495`); inflow
accumulator follows the `mInflow` pattern (`sssurfacefield.h:232`, applied in
`ssgranular.cpp:158-196`); `mPuddle` becomes the render view of `h` at the
`updateWindow()` write (`:1804`) under `SSAtmoFlowSim`; 4-deep persistent-mapped PBO
ring with `glFenceSync`. `SSAtmoFlowSim`'s ladder post-absorption: 0 off / 2 dynamic
(the static tier is the shipping runoff path's job). Cross-gate sink: with FlowSim
off, P5's melt credits `mPuddle` directly. Tests: `twin_flowcore_pipes`, T7.

**P4**: wall-hit branch at the two landing resolves (`:989-995`, `:1034-1040`),
classified by a worldfield column/span test at the landing point (never
`resolveColumn`'s depth derivative); wall ledger (`sswallfilmcore.h`): sparse
per-region map keyed `(azimuth&0xF | band<<4 | along<<12)`, anchored to the region
origin projected on `drip_t` with wind shear excluded from the key; Nusselt decay,
breakup at `h_crit = 0.15 mm·(1−roughness)` into run seeds in the existing
0.06 × 0.30 m drip cells, runouts credit ground via `depositAt` exactly once;
render side: the drip block in `ssSurfaceNormalF.glsl` takes its phase from the
ledger's run distance (the shader's `SS_DRIP_AZIMUTHS` re-declaration at `:208` is a
per-unit const — note, not LOCKSTEP). Tests: `twin_wallfilm`, T8. Cross-gate sink:
with `SSAtmoWallFilm` off, wall-landed mass stays a splash (current behaviour).
