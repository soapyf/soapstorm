# Atmo Magic snow: second design round

A second pass over `doc/atmo_magic_snow.md` with five new designs, adversarial reviews, a
synthesis, and the same treatment for the code architecture. Designs E-I each own one axis the
first round's A-C left standing: the fog layer (E), the mass ledger (F), timing and choreography
(G), the camera (H), and geometry (I). Bracketed section numbers reference the main design doc.

## Part 1 - five designs

### Design E - ground-anchored display density (fog-first)

Keep every piece of D's state and transport unchanged. Add one display layer: a per-region 2D
airborne-density texture (~128², RG16F), GPU ping-ponged every frame - advected by the ground-flow
window and relaxed toward a **CPU-authored target** (lift x depth, packed into the ground window's
spare channel, deterministic). Whiteout density = D's band march (structure, occlusion) **x** this
texture (space and time modulation). Drift particles unchanged.

Why it can work where B failed: the flowmap is divergence-free by construction (the pressure
projection guarantees it), so semi-Lagrangian advection over it is stable and mass-preserving
without any solver of our own; and the targets - not the sim - own the ground truth, so the
determinism line holds.

**Wins.** Gust fronts sweep as visible fog waves (B's surrendered win, recovered). Density
persists and decays over seconds after a gust passes - snow in the air does not vanish. Corridor
structure still comes from targets. Cost: two tiny passes and one small texture, per region.

**Adversarial review.**

- *Divergence returns through the back door.* The fog is now sim state; two viewers see different
  swirls. This is the first genuinely divergent layer in the stack (particles are deterministic,
  the field is deterministic). Mitigation is real but must be stated as a philosophy line, not
  hand-waved: **ground state exact, air approximate** - the displayed field is smooth, shares the
  same targets, and converges statistically the way cloud decks do. Nobody standing side by side
  can compare a swirl; everyone can compare a snowbank.
- *Target staleness.* Targets refresh at the field tick (sub-second); the GPU layer relaxes at
  frame rate. A gust arriving between ticks pops in one tick late - invisible against a ramp.
- *Resolution ceiling.* 2 m cells at 128² over a 256 m region; gust structures are ~100 m. Fine -
  but this layer can never show sub-cell fog detail, and must not try.
- *Feedback risk.* If anything read this texture to make decisions, the approximation would leak
  into state. Hard rule required (see synthesis).

### Design F - particle-as-carrier (mass-honest)

One transport representation: particles. Pickup debits mass quanta from the field into drift
particles; saltation is near-ground particle hops (no creep term); landing credits; whiteout
density is *sourced from the particles themselves* - a point-splat pass renders the drift pool
into E's density texture. No field advection, no deposit potential, no split between look and
mass: the fog, the piles and the erosion are all the same snow.

**Adversarial review.**

- *Transport pays for itself in the most expensive currency.* A ground blizzard moves tonnes of
  snow per minute; representing that as particles needs counts no budget allows. D's field
  advection moves mass in O(cells) and spends particles only on look; F conflates the two and
  then must cap erosion to match the particle budget - transport stalls exactly when the storm is
  biggest. This is fatal on its own.
- *Fog quality becomes budget-coupled.* Sparse pools at Low quality splat into a sputtering fog,
  so an analytic floor is needed anyway - at which point there are two sources of truth again,
  F's whole reason for existing gone.
- *What survives:* the discipline of the ledger. Every mass move debits and credits somewhere
  visible; "approximate but flattering" stays a documented choice rather than an accident. And
  the splat idea - enriching the analytic density with actual particle positions at High tier -
  is worth keeping as a garnish, not a foundation.

### Design G - the regime machine (choreography-first)

A deterministic regime state machine over shared time: CALM -> SALTATION -> DRIFT -> BLIZZARD ->
SQUALL, entered on thresholds (ambient speed, snow presence, falling intensity, temperature),
exited with hysteresis gaps and minimum dwell times (~20-60 s). Each regime is a bundle: gust
envelope shape (the existing `gustDepth`/`gustLength`/`gustVeer` parameterisation), rate
multipliers for drift and near-ring, a **whiteout rate limit** (visibility collapses over ~8 s on
squall onset, lifts over ~20 s after), erosion/deposit aggressiveness scalars, an audio bed
crossfade trigger, and the packed-vs-fresh surface look.

**Wins.** The original brief - "effects are introduced and added on" as intensity scales -
becomes an explicit, legible, timed ladder. Blizzards have beginnings; squalls *arrive*; lulls
let the fog lift before the next front. Five bundles replace twenty uncorrelated scalars, and
the soundscape and UI get a clean seam to subscribe to.

**Adversarial review.**

- *Discreteness fights the flowmap's continuity.* A corridor jets above threshold while the
  regime still says CALM. Resolution is a rule, not a compromise: **the regime directs, the field
  decides** - regimes scale presentation and global envelopes only; local lift stays purely local
  and never reads the regime. A 6 m/s alley saltates in CALM; it just does not drag the storm's
  fog and audio with it.
- *Who authors regimes?* The environment track already keyframes weather; authored regimes would
  fight it. Regimes must be **derived** from the same params the resolver already produces, with
  per-preset overrides as the only authoring surface.
- *State across viewers.* Hysteresis is memory, and memory differs for a viewer joining
  mid-storm. Fix: transitions evaluate on fixed tick boundaries of shared time, and the initial
  regime is a pure function of current params - a joining viewer starts at the derivable regime
  and converges at the next boundary. Minor, bounded, documentable.
- *Flapping.* Real weather oscillates across every threshold. Dwell times and hysteresis gaps are
  load-bearing, not polish - without them the machine strobes, and a strobing fog ramp is worse
  than no machine.

### Design H - screen-space storm (camera-first)

No world-space drift at all. A GPU particle buffer (a few thousand) simulated in screen space,
velocity from the world flow projected at the pixel's depth, per-particle depth occlusion,
analytic fog on top. Scouring stays in the field tick. The storm is always at the camera,
everywhere, at every quality tier.

**Adversarial review.**

- *It amputates the design's soul.* The founding observation was that the flowmap knows where
  the wind is faster than ambient, and the payoff is spatial: jets in alleys, calm in lee,
  plumes over rooflines. Screen-space particles projected through depth can wash across a view;
  they cannot pour over an edge or funnel through a gap. What survives is fog only - and fog is
  E's job, done better.
- *View-anchored simulation smears when the camera turns* - the field is screen-stable, the world
  is not; turning the camera drags the storm. World-anchored emission with camera culling (the
  near ring D already has) is strictly better at the same cost.
- *Depth-discontinuity snaps*: a particle crossing a silhouette samples a different depth's flow
  and changes velocity abruptly.
- *What survives:* the honest LOD. At Low quality, the near layer may degrade to a screen-space
  streak sheet - a documented lie the quality tier tells, never the default.

### Design I - drift cards (geometry-first)

Accumulation's 3D read as instanced geometry: one quad per field cell holding snow above a floor,
instanced per region into a single draw, vertices displaced by the cell's depth (bilinear
continuous across cards, so seams mostly close), edge-faded, POM'd, depth-writing. Banks become
real geometry: kerbs genuinely buried, wall-base piles with true silhouettes - the one thing the
deferred POM pass structurally cannot do.

**Adversarial review.**

- *The seam problem is not the seam problem.* Bilinear displacement from one continuous field
  closes card edges. The real failures are sub-cell: cards sit at the cell's Z *average*, so
  cards sink into every bump and float over every dip the capture smoothed away - with visible
  edges now, because the card is geometry. The same sub-cell truth problem POM hides under
  shading, geometry exposes.
- *Object intersection churn.* Cards vs fences, posts, furniture: depth-write quads either clip
  through thin geometry or leave gaps; every edit re-churns the instance buffer.
- *It is a second accumulation representation.* POM look everywhere, cards where deep and near -
  two systems to keep agreeing about where the snow is, and the agreement problem is exactly what
  the field exists to solve. Cards re-fragment it.
- *What survives:* the confirmation that the field is already the right displacement source. If
  real geometric drift is ever built, it reads this field, and nothing in D's data model changes.
  Recorded as the sanctioned future geometry phase - not scheduled, because the silhouette gap it
  fills (wall bases, kerbs) is where the repose-banked field is smoothest and least silhouetted;
  the visual payoff is narrower than it looks.

## Synthesis S2

D remains the skeleton. Taken: E's display layer, G's regime machine, F's ledger discipline and
splat garnish, H's LOD fallback. Rejected: F's and H's cores, I entirely (with the door left
open). Rules the synthesis adds:

1. **Ground exact, air approximate.** The field, regimes and lift are exact, deterministic
   functions of shared time and captures. The display-density texture is the one approximate
   layer: GPU-relaxed toward CPU-authored targets, never read by any state-owning system, and
   never fed back into spawning, erosion or deposits. One-way flow: field -> targets -> air.
2. **The regime directs, the field decides.** Regimes scale presentation (fog ramps, near-ring,
   audio, surface pack look, envelope shape) and never gate local lift. Regimes are derived from
   the env resolver's existing outputs, hysteresis + dwell are mandatory, transitions evaluate on
   shared-time tick boundaries.
3. **Mass ledger stays as in D** - field advection for bulk transport, mass-honest drips for
   cascades - with F's splat as a High-tier enrichment of the density texture only.
4. **Low-tier fallback**: the near ring may degrade to a screen-space sheet. Never elsewhere.

What this recovers from round one's "genuinely given up" list: spatial gust waves in the fog
(E). What remains given up: sub-cell fog detail, geometric drift silhouettes (I), and per-viewer
identical air swirls (E, accepted and bounded).

**Adversarial review of S2.**

- *Two new subsystems is complexity creep.* Mitigation is that each owns a named, isolated win
  and neither touches state: the density layer is ~2 mini passes and a texture; the regime
  machine is an enum, five scalar bundles and hysteresis. The dangerous part is the interaction
  matrix, and the two rules above cap it: regimes multiply targets; the air layer reads targets
  and nothing else. No cycles.
- *The density layer needs the divergence-free argument to hold at the window's margins*, where
  neighbour tiles overlap. The flowmap's margin overlap already keeps wind continuous across
  borders; advecting two overlapping windows independently gives slightly different density in
  the overlap - visible as a soft seam only during cross-border gusts, and the region border is
  already a documented seam for everything else.
- *Regime onsets must never fight the whiteout ramp they drive.* The ramp rate-limit lives in the
  regime bundle; the whiteout pass reads a smoothed intensity, not the raw regime. If a squall
  onset collides with a gust spike, the ramp rate clamps and the spike shows in the corridor term
  only - the pass must take `min` of the two demand curves, not sum them.
- *F's splat enrichment* must not let particle sparsity read as fog holes at High tier: splat
  adds onto the analytic floor, never replaces it.

## Part 2 - four architectures

The design is now S2. Four ways to structure the code. The variation axes: where transport lives,
what runs on the CPU vs GPU, and how determinism is enforced in the tick.

### Architecture W - in-place extension

The existing plan: `SSSurfaceField` grows lift/creep/deposit and the snow pass; `SSWhiteout`
owns the density layer; the regime machine lives in `SSAtmoMagic`; `SSPrecipSim` gains
`updateDrift`.

**Review.** Zero indirection, pure house style - and a god-class trajectory. `sssurfacefield.cpp`
is already ~1,500 lines; S2 adds roughly +400 across transport and passes. The transport rules -
the one genuinely subtle physics in the system - would be buried mid-file, untestable in
isolation, and phase 6 (creep + shed re-feed) would reopen the same wound. The regime machine in
`SSAtmoMagic` (already large) has the same smell but less of it.

### Architecture X - transport extracted

New `SSGranularTransport` (`ssgranular.h/cpp`): a pure, stateless-stepped subsystem -
`step(Field&, const Geometry&, const Params&, F32 dt)` - owning lift, creep, deposit, spill and
the hysteresis; `Params` is a plain struct assembled once per tick (preset fields, settings,
regime scalars, gust scalar). `SSSurfaceField` becomes storage, windows, passes and shed
plumbing; it calls the transport inside `tick()` and routes `depositAt` through it.

**Review.** The subtle physics gets one legible file with a signature you can reason about;
deterministic and unit-testable without a renderer; phase 6 lands in the same file instead of
reopening `tick()`; regimes flow in as scalars without transport knowing they exist. Cost: one
indirection, and the field arrays must be handed over as references (fine - same translation
unit family, no accessors in the inner loops). The house has no precedent for a stateless
step-module, but `SSGranularTransport` is small enough that style is not a real objection.

### Architecture Y - GPU-resident field

Lift/creep/deposit as compute passes over the field window texture (ping-pong at field
resolution); CPU keeps a shadow for queries, synced by periodic readback; particle deposits
splat via compute; the field tick's CPU cost drops to the readback.

**Review.** The benefit is small - the tick is already throttled, budgeted and O(cells) with
cheap math; nobody is starved of CPU here. The costs are structural: **GPU float rounding breaks
S2's "ground state exact" promise per vendor** (the whole round-one rejection of B, reincarnated
through the accumulation path), query freshness couples to readback cadence, and three new
failure modes (sync, format, split-brain between shadow and texture) arrive to save a
millisecond. Rejected as a baseline. What survives: the display-density layer is already GPU
(S2/E) - Y's instinct is right about *air*, wrong about *ground*.

### Architecture Z - fixed-step core with events

Formalize the whole weather tick as a fixed-step deterministic core (`SSWeatherCore` stepping at
exact boundaries of sharedTime), owning field + transport + regimes, emitting events (regime
changed, squall onset) to subscribers; presentation systems poll the core.

**Review.** The disciplines are right and the top-level class is wrong. Right: the field tick
must step in fixed quanta of shared time (transport must never vary with frame rate - the
current `mTickAccum` already accumulates and wants formalising); regime transitions want an
event seam so soundscape/UI do not poll; presentation-polls-state is already the house pattern.
Wrong: a new top-level owner splits rain/puddle logic (which lives in the same tick) across two
tick orders, for a boundary that already exists inside `SSSurfaceField::idle`. The refactor buys
architectural purity at the price of destabilising working systems.

## Architecture synthesis

Take X's extraction, W's layout otherwise, Z's disciplines, Y's scoping:

- **`SSGranularTransport`** (`ssgranular.h/cpp`, new) owns lift, creep, deposit, spill,
  hysteresis; `step(Field&, const Geometry&, const Params&, F32 dt)`. Pure, deterministic,
  testable. `SSSurfaceField::tick()` assembles `Params` (including the regime scalars) and calls
  it; `depositAt()` routes through the same repose logic.
- **Fixed-step discipline.** The field tick steps in exact `TICK_DT` quanta of sharedTime
  (accumulator formalised), so transport is frame-rate independent and identical across viewers.
  Regime transitions evaluate on the same boundaries.
- **Regime machine in `SSAtmoMagic`** - derived, hysteresis+dwell, bundle scalars - with a
  minimal change signal (a bounded subscriber list: soundscape, sim floater stats, whiteout
  ramp). Not a general event bus.
- **`SSWhiteout` owns the density layer** (targets in, relax out, never read by state) - the
  only GPU-resident weather state in the system, by explicit exception.
- Everything else per the existing architecture doc: drift pool in `SSPrecipSim`, ground window
  on `SSWindFlowMap`, snow pass beside the wet pass.

**Adversarial review of the architecture synthesis.**

- *The Params struct is the seam to defend.* It must stay a plain aggregate of floats/bools -
  no LLSD, no settings lookups inside the transport, no back-pointers. The moment
  `SSGranularTransport` reaches out for settings or singletons, X's testability is gone and it
  is W again with extra steps.
- *The event seam wants to grow.* Regime change is the only event; if a second event type
  appears, promote it to a real pump consciously, not by accretion. Cap named at review time:
  soundscape, floater stats, whiteout.
- *Fixed-step cadence changes existing feel.* Rain/puddle today step on whatever accumulator the
  frame hands them; locking to shared-time quanta subtly changes dry-down timing. Acceptable -
  it is a change toward determinism - but it lands behind its own setting-safe default and gets
  a test-plan line, not a silent ride-along.
- *Who owns `depositAt` validation?* The transport owns the repose cap; `SSSurfaceField::depositAt`
  becomes a thin forwarder. Any future caller bypassing it (there will be one) writes to `mSnow`
  directly and breaks the ledger - guard with a comment and a debug-only assert on monotonic
  repose compliance, the cheapest damper available.
- *Residual risk, unchanged from S2:* the one-way rule (field -> targets -> air) is enforced by
  convention. The enforcement point is code review, and the rule is written in both docs so the
  reviewer has something to point at.

## Deltas applied to the canonical docs

- `atmo_magic_snow.md`: whiteout section gains the display-density layer; new section for regime
  choreography; settings and cost tables updated; the round-one "genuinely given up" bullet
  (spatial gust waves) corrected.
- `atmo_magic_snow_architecture.md`: `SSGranularTransport` added to the module map and APIs;
  fixed-step discipline; regime machine + event seam; density layer ownership; phase mapping
  updated.
