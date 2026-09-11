# Atmo Magic: phase 8 — making the storm show visible

Status: **design + findings** (2026-09-06, from the first viewer build of phases 6c–7). Depends on `atmo_magic_storm_dynamics.md`, `atmo_magic_far_clouds.md`, `atmo_magic_wind_profile.md`, `atmo_magic_debug_views.md`. Harness: `V:\Scratch\atmo\PLAN.md` §17.

## 1. What the first build showed, and why

The scheduler, coupling and debug views work as designed; almost nothing of the *show* reaches the eye. Each observation traced to a cause:

| Observation | Cause | Fix |
|---|---|---|
| Forced storm under the editor's preview "loops a few seconds back and forth", never matures | The preview map's reference instant was the 2 s memo bucket start, re-latched every bucket; the cue's wall time jumped 2 s every 2 s, so the storm's age stood still and its position sawtoothed | 7d: reference latched once when the preview turns on or changes phase (superseded by cycle time (7e); mPreviewRefTimeS is gone, see section 2 and `SSStormCells::mCycleRefS`/`mWallRefS`) |
| The funnel vanishes on every cue/offset tweak | Kind and life window were hashed from the cell id; a new offset is a new cell, usually one whose hashed window is closed at the cue | 7d: a forced tornado/waterspout/anticyclonic pin **guarantees** its funnel — slot 0 is mesocyclonic with a fixed window (age 0.30–0.75, touchdown 0.39–0.64, cue at 0.5) |
| Funnels seem tied to "tile edges" | Storm cells are **entities**, not tiles: the 2.9 km lattice only seeds where and when a cell is born (its potential is a coherent fbm field sampled per cell, not salt-and-pepper); the cell then moves closed-form and its funnel is its child. Nothing crosses a tile edge. What was seen was the re-hash above: a new offset seeded a new cell with a new hashed window. The funnel does descend (condensation front over the first 20% of its life), rope and lift; that was invisible because forced funnels never survived a tweak and spontaneous ones rarely form | 7d fix above. Cosmetic follow-up: the V2 lattice-potential tint is drawn per tile (nearest-neighbour) and reads as aliasing; smooth it bilinearly in the view (8a, display only) |
| Spontaneous heroes far away, dying upwind, no funnel even when close | The old law passed 500–1000 m from the anchor after a fixed 1.5–2 km spawn, so a slow wind meant the hero died before arriving; the CELL centre was composed, not the funnel (whose contact sits up to a radius away); and the funnel window was hashed, so a hero usually carried none during the pass | 7f (§3c): pass 0–2 km skewed to 0 (median 300 m), closest approach at mid-life whatever the wind, the funnel contact is what passes, and a hero clearing the meso gates gets its window pinned around the pass. Ladder: distribution unit test, 300-seed scenario, cross-section, flyby overlay + histogram |
| Blurry rectangles under the deck | Rain shafts: 286 m-wide isolated pillars at 0.55 alpha with a 45% solid core, one per qualifying cell | 7d: curtains overlap into a sheet (width 0.62 cells), core 10%, streak erosion 0.65, alpha 0.35→0.03 |
| Squall lines never seen | 6% of severe epochs = one per ~8 h of severe weather; and a line is 7 ordinary cells fighting for 4 coupling slots, so it never reads as a wall | 7d: odds 0.18. 8a: authored squall kind + a line-band coupling primitive |
| Puffs do not rotate with the storm | Rotation only reaches the shader's striation read; puff positions are lattice-hashed and never displaced by the meso | 8b: swirl frame term |
| No anvils | Anvil is a shaping weight on the existing column (flatten + tower window); nothing spreads downwind at anvil altitude | 8b: anvil plume tier + mammatus |
| Mid-field clouds patchy, rows visible; wants 20 km horizon | Thinning keeps one puff per cell; 260 m lattice shows as rows in the squash; nothing beyond 9.8 km | 8c: horizon deck |
| Non-forced cells move in real time but warp on preview change | Expected: preview re-decides which epochs are severe; ages stay wall-clock | Documented (§2) |
| Generator does not time a severe storm's arrival | The bias raises peaks; it never authors *when/where* a storm passes | 8a: the roll writes a cue |

## 2. One clock: the scheduler runs on cycle time (user, 2026-09-06)

Until 7d the storm scheduler mixed two clocks: births, ages and positions ran on the raw wall clock, while the weather was read at a day-cycle *phase*; the editor's preview only re-mapped the phase, so scrubbing changed which epochs were severe but could not show a storm at its peak, and a forced cue needed a second inverse map (lesson 24). The user's rule replaces all of that: **animation runs on the wall clock; position and state run on the day-cycle timeline.**

- **Cycle time** `tau = utc - dayOffset` (seconds along the track's own timeline; `phase = frac(tau / dayLength)`). It ticks at wall rate, so storms still move at wind speed in metres per wall second, and it is identical on every client sharing the asset (offset and length are asset constants) and the seed - true sync without any extra protocol.
- The scheduler hashes epochs, births, lifetimes and forced cues on `tau`; `phaseAt(tau)` is the one real map; `wallTimeAtPhase` is its one inverse. Epoch boundaries therefore sit at fixed phases (a 4 h day is exactly 8 epochs), so the same phase always means the same lattice epoch - when the day length is a multiple of `EPOCH_S` (30 min); the day-length slider steps 0.25 h, so half its settings are not, and on those an epoch boundary drifts across phases over the day instead of landing on the same one every cycle.
- **Preview** substitutes the clock, not the map: when the slider moves, `tau_ref = cycleTimeAtPhase(P, near now)` is latched and `tau(now) = tau_ref + (now - now_ref)`. Everything downstream - which cells exist, their ages, positions, the forced storm's state, the funnel's descent - is the exact state of that phase, and time flows from it. Scrub to the cue: the tornado is on the ground. Scrub earlier: it is building.
- **Animation** (puff drift accumulator, funnel rotation texture, debris particles, lightning buckets, sound) stays on the wall clock: smooth, framerate-independent, not part of the synced state.
- Consumers: `SSStormCells::now()` *is* `tau`; vortices, virga, the V2/V7 views and the coupling read it. Nothing reads `sharedTime()` for storm state any more.

This supersedes the preview map (`previewPhaseAt` / `previewWallTimeAt`) and 7d's reference latch; both are removed. Lesson 31 in PLAN.md.

## 3. Phase 8a — authored severe weather that actually arrives

**Principle (user, 2026-09-06): a generated squall line IS the weather change.** Clear sky → the line arrives → hours of heavy showers. So the generator does not roll curves and then hope a line happens to pass; it authors the line's arrival as the cue and shapes the precipitation/moisture/convection curves to *follow* it: dry and clearing before the cue, a sharp onset at the cue (the gust-front shelf and the wall arrive together), heavy precipitation sustained for the authored duration, then a slow taper. The cue is the source of truth; the curves are derived from it. The same holds for an authored tornado day: the convection peak is placed at the cue.

1. **Squall as an override kind.** `mStormOverride` gains `"squall"` (kind 5). `SSSquall::forcedLine(seed, override, anchor, windAnvil)` composes a line event whose leading edge crosses `anchor + offset` at the cue (origin = crossing − motion × lead), with the ordinary member template; the scheduler's line hook returns it for the epoch containing the cue − lead (replacing that epoch's hashed decision), so it is deterministic and previewable like a forced cell.
2. **Line-band coupling.** A new uniform in `ssstormcouplecore.h`: `LineBand { origin, dir, halfLen, bandM, strength, shelfM }`. `towerWindow`/`anvilWeight` take `max()` with a segment-distance field so the deck raises a **continuous wall** along the line (not 7 discs), with a lowered **shelf** (gust-front) band `shelfM` ahead of it along the motion. Replicated in `ssVolCloudF.glsl` (twin test), read by the bake through the existing quantised sample. Costs one segment-distance per cell.
3. **The severe-day roll authors a cue AND the curves that follow it.** `randomize(severeDay)` picks the event kind (squall line or supercell/tornado day), places the cue phase inside the rolled afternoon window, writes the forced override (kind, cue phase, offset 1.5 km *upwind* at the anvil wind of that phase so the leading edge / flyby crosses the region at the cue), and then RESHAPES the curves around the cue: precipitation intensity stepping from ~0 to the rolled peak over `ONSET_PHASE` (≈ 5 min of day) at the cue and holding for the rolled duration; moisture/convection ramping to their peaks just before the cue; wind speed/veer jumping at the cue (the gust front). Weathergen already owns curve shaping at authoring time, so this is the same code path with the cue as its anchor. The roll summary names the event and its arrival time.
4. **Line-driven precipitation.** The line-band coupling (item 2) also feeds `precipIntensity` for cells under and just behind the band (`SSStormCouple::precipShift` idiom), so the rain shafts and the particle rain start exactly where the wall is, even when the authored curve has not yet ramped.
4a′. **The authored floor is the authored severity** (build finding, 2026-09-06: a top-down look showed the line's seven members alive at intensity 0.00). The 8a floor of 0.5 moisture × 0.5 convection scored 0.25 against the 0.245 spawn threshold: the members spawned, which was all the rung asserted, but intensity is a ramp from 0.245 to 0.65 and every lifecycle field, the line band's strength and the shafts' drive scale by it, so the wall arrived and did nothing. The floor is now `AUTHORED_FLOOR_MOISTURE/CONVECTION` (0.85 each, product ≥ full-intensity score by static_assert): a line member is a full-intensity storm by construction, and the pinned cell's draw is raised the same way. The rung asserts intensity ≥ 0.999 on every member through the pipeline. Lesson: a spawn test must assert what the spawn *drives*.

4b. **A line is the show.** While any squall-line member is alive the spontaneous hero search is skipped (7f review F11): a composed hero would otherwise be dragged to the anchor beside the wall, into the very band the line's suppression keeps clear. An authored tornado cue would still win, but the override is one HOLD curve, so only one kind is active at any instant: a squall held across its life excludes a tornado pin over that window (8a audit). Line members themselves are never the hero, so the line stays a line.
5. V2 draws the line band and shelf (and smooths the lattice-potential tint bilinearly - display only); V5 marks the authored cue.

## 3c. Phase 7f — the spontaneous hero passes close and on the ground (user, 2026-09-06)

"More likely to pass close than far, like the lightning strike-now button: 0 m–2 km skewed toward 0, 50% within ~300 m; and they must not die far upwind." Four changes, all in the two cores, wired by the shell:

1. **Pass law.** `pass = 2000 m × u^2.74` with `u` the hashed pass fraction: median 300 m, quartiles ≈45 m and ≈909 m, never beyond 2 km. The old floor of 500 m is gone: the show may pass over the region.
2. **Arrival at maturity.** The closest approach happens at age 0.5 regardless of wind: `spawn = |motion| × 0.5 × lifetime`, clamped to 200 m–30 km (the hero is cull-exempt, so it may be born beyond the 12 km field and travel in; a slow wind spawns the hero nearer; when the clamp bites the closest time follows the clamp). No hero dies upwind any more; the "wandered off" cases were heroes born 2 km out in a 2 m/s wind.
   *Calibration (2026-09-06, from the scenario's probe over 2,400 hero draws):* the first cut capped spawn at 2 km, so a 13 m/s storm reached the anchor two minutes after birth (age 0.07) and no funnel window could open; and the gate mapped score to intensity with a smoothstep to 1.0, so typical severe scores of 0.3–0.5 gave intensity 0.0–0.15 and the mesocyclone gate at 0.55 was unreachable (phase 5's own test had to hunt for a special anchor to find one crossing). Fixed: spawn cap 30 km (a 12 km cap still left long-lived heroes arriving at age 0.21, before the meso window opens); full intensity at score 0.65; the composed hero's intensity floored at 0.7 (the show principle); mesocyclone gate 0.35. Intensity is a global change: every background cell gets stronger towers, anvils and radii too.
3. **The funnel passes, not the cell.** The hero path is composed so the slot-0 vortex's *contact point* (cell centre + its hashed offset at the closest age, 25–55% of the cell radius away — and the same offset now applies to every supercell funnel in the field, not only the hero) is what lands at the closest point. The shell hands the scheduler that offset through a hook, computed from the vortex core at the closest age.
4. **Pinned window.** When the hero clears the ordinary meso gates, its funnel window is pinned to start 0.12 of life before the pass and last 0.35, so touchdown spans the pass. The gates still decide *whether*; only the *when* stops being hashed.

Ladder: unit (distribution quantiles, contact identity, degenerate motion), cross-section along the path over 200 seeds with the old law as a failing control, a 300-seed scenario reporting the touchdown share at the pass and the gate-failure shares, and a visual overlay of 60 flybys over 300 m / 1 km / 2 km rings with a histogram. The review reads the measured numbers and names the one constant to move if the touchdown share is under 85%.

## 3b. Phase 8e — rain shafts as skewed fog, not pillars (from the first build)

The 7d constant retune is not enough; the user's spec is exact and right: a shaft is **a puff cloud stretched vertically and smoothed into thick fog**, skewed to the same fall angle as the precipitation, reaching from *inside* the deck base down to the water/ground, with no visible join at the base.

1. **Skew — following the wind profile** (revised 2026-09-06: "segment the virga in parts and follow the wind profile"). A drop released at the deck base drifts by the *integral* of the wind profile over its fall, not by one base wind: faster aloft, slower and veered inside the boundary layer. The curtain's cards are chords of that curved trajectory, each card's top and bottom offsets read from the same integral at its own altitudes, so the stack bends continuously. Two caps bound it: the slope never exceeds 45° and the total never exceeds 900 m, which is also what the walk pad covers. The straight-slant rule below is the uniform-profile special case and stays as the test's reference. Each curtain is a slanted sheet: the horizontal offset at altitude `z` below the deck base is `skew(z) = wind_base × (baseZ − z) / v_fall`, the same rule the particle rain uses for its landing shift (`ssprecipitation.cpp`, wind × fall time), with `wind_base` the curve-resolved wind at the deck base (never the eased per-client wind) and `v_fall` the active precipitation preset's fall speed. Cards become parallelograms (top and bottom offset by `skew(zTop)`/`skew(zBot)`), each evaluated from the SAME `skewOffsetM` function at its own height, which is what keeps consecutive cards' shear chains continuous — but not a claim that the whole stack reads as one continuous slanted sheet (F4/F5, 2026-09-06 review): the slope cap (`SKEW_RATE_MAX`, 45°) bounds any one card's own shear to at most its own height, and the one card whose span straddles the magnitude cap's own knee (`SKEW_CAP_M`) is a chord of that piecewise (linear-then-clamped) path rather than two points on a straight line — its overlap partner, evaluated at its own zTop/zBot, generally disagrees with it across their shared band. A stated residual at that one boundary. Magnitude capped (`SKEW_CAP_M`) and **added to the walk pad** (lesson 16).
2. **Embedded top.** The stack starts `min(0.3 × thickness, 150 m)` *above* the deck base, inside the puffs, with alpha ramping in over that embed so the curtain emerges from the cloud rather than hanging off a hard line.
3. **Reach.** The far ground lift is retuned to bite only well beyond the knee (start 1.5×, full 3×, max 30% of the span); near and mid curtains reach the water/ground reference. Over void water the ground reference is the water height already (water-floored average).
4. **Body.** The shader stops treating a shaft as a soft box with a streak. It reads the *puff's own* cloud noise (the two vertical triplanar planes, weighted by the card's facing) at a vertically stretched coordinate (z × 0.25) so the texture is the deck's, pulled downward; the soft box only masks the card, the noise decides the silhouette, the streak adds fall. Alpha profile unchanged (0.35 → 0.03).
4a. **Width and texture follow intensity** (user, 2026-09-06). Photographs show three kinds: wispy filaments under light rain, walls hundreds of metres to kilometres wide under dark heavy rain, and lines under squalls. The curtain's drive (the same precipitation × presence figure the cell qualified with) now sets both: half-width at the base from 57 m (wispy) to 195 m (heavy), so heavy curtains overlap their 260 m neighbours by 1.5× and a dense storm reads as one wall, while wispy ones stand alone; and, carried to the fragment in the card's vertex colour, the body floor and streak swing, so a light-rain curtain is a few eroded filaments and a heavy one a near-solid dark wall with streak texture. Lines come from 8a's line band, which raises drive along the segment. Alpha already scales with drive.
4a′. **One phenomenon, evaporation from below** (user, 2026-09-06). Rain shafts and virga are the same thing with the same shading; virga is only the case where the precipitation evaporates before it lands. So the fixed vertical alpha profile (denser at the base, wispy at the ground) is gone. Alpha is intensity × density across the volume: `alphaFor(drive)` times the fragment's body noise, times an **evaporation mask** that erodes the curtain from the bottom up. The mask's height follows drive (light rain ends two thirds of the way up, heavy rain reaches the ground) and its edge is jittered by the streak noise so the bottom is ragged wisps of different lengths, never a line. Cards wholly inside the evaporated zone are not emitted. The far ground lift is a separate, geometric workaround for the squash fold and is documented as such.
4b. **Lighting (user, 2026-09-06): the curtain lights exactly like the cloud it hangs from.** In photographs virga reads as part of the cloud base, smeared downward and fading, because it is lit as the base is. So the shaft fragment uses the *puff's own* fake-sphere normal reconstruction (one GLSL function for both branches, so they cannot drift), with the normal tilted downward like a puff's lower rim by a droop that follows the curtain's height fraction (0.35 at the base, 0.85 at the ground, continuous across stacked cards), and the same form/buried inputs the veil underside uses. The shared lighting tail then does the rest: wrap light, sun facing, strike lighting, all identical to the deck's. Nothing about a curtain's shade is its own.
5. Twins for the new box/noise call site; visual side view with skew; V4 draws the skew vector.

## 4·0 The architecture this settles: the deck is a container, entities are operators on it (user, 2026-09-06)

The volumetric field is the **medium**: a deterministic fabric of puffs, a pure function of (air-frame position, seed, weather, cycle time). Entities do not replace it; they **act on it**, and everything an entity does is one of four operator kinds, all evaluated per frame in closed form (never integrated), all bounded, all replicated wherever the field is read:

| operator | what it does | existing examples | coming |
|---|---|---|---|
| **displacement** `D(p, t)` | moves the fabric: puffs, and every observer's read of the field, by a bounded closed-form offset | the hero frame shift, the wind profile's O(z) lean, the virga skew | the supercell **swirl**; the **tornado**: puffs within its reach rotate about the axis, are pulled inward and slightly **down**, so the deck's own base bulges into the lowering and wall cloud around the funnel; the shelf's lift |
| **density / occupancy** | modulates presence, tower, anvil, drive, coverage over an area | the storm coupling (tower window, anvil), the line band (wall + shelf), precipitation intensity | **kilometre-wide rain-cloud entities** (nimbostratus cores: coverage, darkness, drive over their footprint), anatomy carves (a tower is a cylinder, an anvil a disc) |
| **emission** | adds geometry the fabric has no cell for | rain shaft cards, the funnel's collar quads, Tier B bodies | anatomy parts (anvil sheet, mammatus lobes, wall/tail cloud, flanking line) |
| **shading** | changes how the fabric is lit or coloured locally | meso gloom (the quantised storm sample), the veil underside form | rain-core darkness, anvil sheet form |

Rules that make this safe (they are the lessons the harness already enforces): every operator is a pure function of world state and cycle time, never of camera, settings or frame time; displacements are bounded in displacement and applied by the producer, inverted by every observer in the **reverse fixed order** (a composition rule: hero shift, then swirl, then tornado pull, then O(z); observers undo O(z), tornado, swirl, hero — lesson 12 generalised); the operator set per frame is a small fixed-size list (the uniform slot arrays) so the fragment gate, the shadow bake and precipNoiseAt read the same operators the builder applied; operators owe a twin test where they are replicated.

**The tornado, in these terms:** the funnel itself stays an emission (the collar quads, the vortex core's shape and lifecycle). What changes is that the deck's puffs near the axis are *operated on*: a swirl about the axis whose angle falls off with radius and vanishes at the reach, an inward radial pull, and a downward pull strongest near the axis and near the base, so the wall cloud and the lowering are the deck's own fabric being drawn into the funnel's top rather than a separate object hung beneath it. The condensation front then reads continuously from the deck through the wall cloud into the collar funnel.

**Rain-cloud entities:** a `Cb`'s precipitation core and a plain nimbostratus rain cell are the same density operator at different scales: a footprint (ellipse, downshear of the updraft for a Cb; kilometres wide, slow, for a rain cell) that raises drive and coverage and darkens, moving closed-form with its wind. The rain shafts, the particle rain and the lightning cluster inside it. This is what "kilometre-wide rain cloud" means in the model: not a shape, a footprint operator on the container.

**Thickness is two things (user, 2026-09-06).** Today one number, the cloud field's `mBaseThicknessM`, drives three unrelated things: the container's geometric depth (how far the puff column extends above the base), its optical density (the shade term `thicknessTerm`, the puff size gain, the veil alpha), and the height the towers grow to (the tower window scales with it). In the container model those separate: (1) **deck depth** — the stratiform layer's geometric extent, a container property, hundreds of metres; (2) **deck density** — how opaque and dark the fabric is per metre, a container property authored on its own curve (a thin grey stratus and a thick dark nimbostratus can have the same depth); (3) **top** — where the fabric reaches above the deck, which is what entities push (tower operators, anatomy parts), measured from the base and never scaled by the deck's depth. Consequences for 8b-1: a `mBaseDensity` curve in the cloud field cube (keyframed, generator-authored: the wall darkens by density, not by depth), `SSDeckShade::thicknessTerm` fed by density × depth rather than depth alone, the tower/anvil heights expressed in metres above the base from the operator, and `mBaseThicknessM` left meaning depth only. 8f's "thin before the cue, thick at the cue" authoring moves from depth to density once the curve exists.

**Removed with this model (user, 2026-09-06):** `SSAtmoCloudTessellation`, the refinement LOD that grew hashed child and grandchild puffs around near puffs. Near-field detail is what entities are for; a finer sampling of the same field competes with them for the budget and reads as more of the same. Setting, XUI row, constants and builder block are gone.

## 4. Phase 8b — storm anatomy: the cumulonimbus as a quasi-entity (revised 2026-09-06)

**The user's observation, adopted as the design:** the volumetric field is a one-layer height field (per 260 m cell: base, top, tower weight, anvil weight; puffs texture the column). A cumulonimbus is a multi-layer *shape*: a tower 2–4 km wide reaching 8–14 km, an anvil sheet at the equilibrium level spreading downwind, mammatus under the sheet, an overshooting dome, a shelf at the gust front, a wall cloud with a tail cloud feeding it, a flanking line. The field cannot express structures that live on several altitudes at once by modulating per-cell scalars, and asking it to "spontaneously" produce them means encoding geometry into heights. So the storm-cell entity, which already owns position and state, gains **anatomy**: a deterministic procedural shape model in the cell's own frame, emitting its own puffs and carving the field so the two agree. The field stays the fabric for stratiform and fair-weather decks; entities own the organised structures.

**Archetypes** (one per coupled cell, chosen from its state): ordinary cumulonimbus; supercell (tilted updraft, wall cloud, rear-flank clear slot); squall-line member (the line archetype: a continuous shelf and wall along the segment, which is what makes the wall read as a wall rather than seven discs); towering cumulus (congestus, before the anvil forms); later: cumulus clusters, lenticular over terrain waves.

**Parts**, each a closed-form primitive in cell-local coordinates evaluated per frame from `(cell params, age01, seed)`, never integrated:
1. **Updraft tower**: a vertical-ish core (tilted downshear by the wind profile's O(z)), radius profile vs height from the lifecycle (radiusAt × a per-height envelope), puffs on rings around the axis with hashed jitter, rotating about the axis for supercells (the swirl: θ = ω·age, weight peaking mid-radius, zero at the rim, so it obeys the frame rules), and the user's **updraft loop**: puffs carry a periodic vertical offset up the tower, fading in at the base and out at the top.
2. **Anvil sheet**: a plume at the equilibrium level from the tower top, spreading downwind along the anvil-level wind with a fan half-angle, thin (sheet-shaded, `SSDeckShade` veil form), radius growing and alpha fading with downwind distance up to `ANVIL_REACH_M × anvil weight`; the **overshooting dome** on top while `overshoot > 0`.
3. **Mammatus**: Worley lobes hanging under the anvil sheet while `mammatus > 0` — small downward puffs at lobe centres with high buried.
4. **Base structures**: the **shelf** (a laminar wedge ahead of the outflow, the line's gust front), the **wall cloud** (a lowered block under the updraft, where the funnel hangs — the 7f contact offset already names the point), the **tail cloud** (a low band feeding the wall from the precipitation side), the **flanking line** (a row of towers up the inflow side).
5. **Precipitation core**: the rain shafts already hang from the field's cells; the archetype declares its own core (a downshear ellipse of high drive) so the curtains, the particle rain and the lightning cluster where the anatomy says.

**Agreement with the field** (the replication discipline still holds): every part also carves the field's own inputs, cheaply and analytically — a tower is a cylinder, an anvil a disc, a shelf a wedge — so the fragment gate, the shadow bake and precipNoiseAt see occupancy where the parts are, without noise reads. Parts are placed through `placeWorld` with the hero shift like every puff, and the O(z) lean applies per part altitude.

**Budget and LOD**: parts emit puffs into the same deck (same sort, same shader, same budget backstop) with a per-archetype budget concentrated on the hero (a few hundred puffs) and thinning by distance the way Tier A does; beyond 5 km an archetype collapses to a handful of large bodies (tower body, anvil body) the way Tier B does; beyond the field the horizon deck (8c) carries its silhouette.

**Sequence**: 8b-1 the anatomy core (`ssanatomycore.h`: archetype selection, part primitives, closed-form placement, carve primitives, LOD) with the ladder (unit, cross-section through a tower, visual side/top views of each archetype at four ages); 8b-2 the tower + anvil + dome for the hero; 8b-3 mammatus, shelf, wall, tail, flanking line; 8b-4 the line archetype replacing the line band's disc coupling. The swirl, anvil plume and updraft loop below are now parts of this, not separate features.

### 4 (original). Storm anatomy — the three tricks, now parts of the archetype

1. **Swirl.** A bounded rotation displacement about each coupled cell's centre: `R(p) = rotate(p − c, θ(t) · w(|p − c|/r))`, `θ = ω · age`, `w` = smooth bump (0 at centre, peak mid-radius, 0 at the rim). Displacement is bounded by `2 r w_max` and is **closed-form in wall time**, so it obeys the frame rules: producer applies it after the hero shift; observers (fragment, bake texel loop, precipNoiseAt) invert it (lesson 12); the shear ring problem (lesson 10) is avoided because `w` vanishes at the rim. Twin + round-trip tests before call sites. Supercells only (`mGate.mSupercell`), sign from rotation.
2. **Anvil plume tier.** For each coupled cell with `anvil > 0`, an extra puff pass at the equilibrium level: puffs placed along the anvil-level wind vector from the tower top, spread with distance (a fan of half-angle ~20°), altitude flat at `top_z`, radius growing and alpha fading with downwind distance up to `ANVIL_REACH_M = 6 km × anvil`. Placement through `placeWorld` with the frame terms; count budgeted like Tier B bodies (a few dozen per cell). Read-only for the bake (anvil casts no ground shadow of its own).
3. **Mammatus.** Under the anvil plume, a Worley-cellular lobe field: each lobe a small downward puff at `top_z − lobe_depth`, positions from a 2-D Worley on the anvil base (the wind-profile doc already names Worley for this); appears only for `lifecycle.mMammatus > 0`.
4. **Tower updraft loop.** The user's trick: in the tower column, puffs carry a periodic vertical offset `z += A · frac(age/T + hash)` with alpha fading in over the bottom 15% and out over the top 15% of the loop. Closed-form, bounded (`A ≤ tower height`), deterministic per puff hash; observers do not need to invert it because it only moves puffs *within* the column the gate already owns (state that explicitly and pin it: the offset never crosses a cell boundary).

## 5. Phase 8c - the horizon deck (a second dome shell, revised 2026-09-06)

**The design in this section was a far annulus mesh at deck altitude. It is replaced** (user, 2026-09-06: "I do prefer
a square tile grid for the veil and stuff because the textures are not squashed, so the concentric circle is
problematic and you nailed it by us using the sky dome instead"). What follows is the shell design; the mesh design is
recorded at the end of the section so the reasoning that rejected it is not lost.

### The reach, from the home body

The horizon is not a constant and should never be authored as one. For an eye at height `h` looking at a deck whose
base sits at `z_deck`, both above a body of radius `R` with refraction folded in as `k = 7/6`, the deck stays visible
out to the sum of the two tangent lengths:

    d = sqrt(2*k*R*h) + sqrt(2*k*R*z_deck)

Against EARTH's radius (6371 km) that is 127 km for a 1000 m deck seen from the ground, 139 km from 20 m up, and
244 km from a 1000 m sky build - measured 126.95 / 139.17 / 243.85 by `tests/shellcore.cpp`, where "the ground"
means an avatar's 1.7 m eye height (at h_eye exactly 0 it is 121.92 km). **CORRECTION, 2026-09-06:** this fork's
default body is NOT Earth-sized, despite the comment on `SS_DEFAULT_PLANET_RADIUS_M` in `ssatmoenvapplier.cpp`
calling it one - it is 5.0e6 m, the radius `atmo_magic_cloud_parallax.md`'s own worked 1.4 degree example uses. A
default track's reach is therefore 0.886x the Earth table: **112.5 / 123.3 / 216.0 km**, which still spans the
"100-200 km" this section was written around. Both radii are named constants in `ssshellcore.h`
(`EARTH_RADIUS_M`, `VIEWER_DEFAULT_RADIUS_M`) and `unit_horizon_reach_default_body` measures the ratio at 0.88589;
a track that authors a home body overrides both. The user's "100-200 km" is the deck's term, not the eye's: at avatar eye height the eye's own
horizon is 5.0 km, well INSIDE the field we already draw, which is why the naive single-tangent formula would have
made the veil worse rather than better. The deck also does not need a cutoff rail: the curvature drop `d^2 / (2*k*R)`
carries it under the horizon ray at exactly that distance on its own (a 1000 m deck is 168 m lower at 50 km, 673 m at
100 km, and has reached the eye plane by ~120 km). Geometry terminates the layer; no constant does.

### Why not a mesh

Three findings, in the order they bite.

1. **Vertex count.** A 520 m square lattice from 10 km to 140 km is about 226,500 blocks against the veil's present
   5,764 - a factor of 39 on a draw the far-clouds doc already flags as one of two dominant per-frame costs.
2. **The polar answer is ruled out on texture, not on cost.** Concentric rings whose radius grows geometrically hold
   every tile at constant angular size and bring the same reach to ~2,400 tiles (19 rings of 128 sectors at g = 1.15),
   but an annular sector is a trapezoid: UV interpolation shears across it, so the veil's pattern would skew and
   stretch differently at a ring's inner and outer edge. The square partition stays conformal and stays.
3. **Depth is worse than vertices.** The squash fold is linear from the knee (1606 m drawn) to `mEffRadius` onto the
   cap (2007 m). Raising `mEffRadius` from 14 km to 140 km compresses the present 10-14 km band from 129 m of drawn
   depth to 12 m - and not just for the veil: puffs, shafts, water and lightning share that fold. A mesh reach of
   100 km is unaffordable in the depth buffer no matter how few triangles carry it.

A dome shell has none of these. It is sampled per fragment from the view ray, so it has no tessellation, no UVs to
shear, and it is painted in the sky pass at the far plane instead of being folded into the depth range.

### The mechanism exists one layer up

`cloudsF.glsl`'s `ss_plane_base` (doc/atmo_magic_cloud_parallax.md) already intersects the true view ray with a
SPHERICAL SHELL centred on the home body, and `ssatmoenvapplier.cpp` already resolves that body's radius from the
applied track's planetary system with an Earth-sized default. Four properties of it are exactly what 8c needs and are
already shipping for the cirrus band:

- **Altitude is a parameter, not a uniform.** `ss_plane_base(alt, scale, ...)` takes the layer's signed height over
  the camera, so a second shell is a second call, not a second copy.
- **Both faces are already handled.** `side = (alt >= 0) ? 1 : -1`; above the shell the intersection switches to the
  minus root and only down-rays hit - the deck seen from above, which is the half the user identified as never having
  been taken advantage of.
- **The layer dissolves across its own altitude.** `plane_fade` ramps out over `SS_THROUGH_LO_M`..`HI_M` (40..300 m),
  with the mapping degenerating as the camera meets the plane "and the deck's own volume takes over exactly there" -
  which is the camera-at-deck-altitude case answered before it was asked.
- **The rim is the tangent elevation.** `ss_deck_edge_fade` dissolves the layer across `sqrt(2*alt/orbit)`, so the
  shell ends at a curved cloud horizon melting into the atmosphere rather than at a smeared seam.

### The deck shell is NOT the cirrus band

Stated because the first draft of this section blurred them (user, 2026-09-06: "no no thats the cirrus cloud layer
that is already rendered above the deck ... otherwise it is still fairly distinctive cloud layers"). The cirrus band
keeps its own authored height, its own drift, its own texture and its own horizon, and it stays ABOVE the deck. The
one case where the two converge is already modelled and stays as it is: `cloudDomeAltitudeMetres` brings the cirrus
band down toward the tops on convection's anvil ramp alone, and moisture never moves it. What 8c adds is a THIRD thing
in the stack - a shell at the volumetric deck's own altitude - and the two layers terminate at visibly different
elevations because their heights differ, which is part of what keeps them reading as distinct layers rather than one
smear.

### What must be restructured first

`ss_plane_base` generalises on altitude and on sign, but three of its constants are calibrated to the cirrus band and
must become parameters before a second shell may call it, plus one gap:

- **`SS_DOME_TILE_M` (32 km)** is pinned against the band's 6 km default height by its own comment. The deck shell's
  feature scale is the field's, not the cirrus layer's, and must come from the deck's own structure.
- **`SS_PARALLAX_DAMP` (0.125)** damps the world-anchored terms to one eighth of the plane-honest rate to match a
  look hand-tuned in the live viewer. The deck shell cannot take that: it has to agree with the TILED veil at the
  handoff, and the tiled veil is world-honest and drift-anchored, so a damped shell would slide against it as the
  camera moves. The shell runs undamped; the damp becomes an argument and the cirrus band keeps 0.125.
- **`SS_DETAIL_LO_M`/`HI_M` (100/250 km)** are the cirrus layer's fine-octave give-up rails and want their own values
  at a deck's reach.
- **`ss_deck_edge_fade` returns 1.0 for `alt < 0`** - a layer BELOW the camera gets no rim melt at all. Seen from
  above a shell has a rim on the same tangent elevation (rays flatter than it pass over the shell entirely), so the
  below case needs the same dissolve or the deck seen from a sky build will end on a hard circle.

Structurally: the intersection, the tangent elevation and the rim fade move into a core (`ssshellcore.h`), the deck
shell gets its own shader that transliterates it, and `cloudsF.glsl` is refactored to call the same core so there is
one authority for the geometry rather than two spellings of it. A twin holds core and both shaders together.

### The handoff is a complement, not a crossfade

The tiled veil already fades OUT over `SSVeil::REACH_START_M`..`REACH_END_M` (10 -> 14 km) via `reachOut`. The shell
fades IN over the same pair as a function of its own intersection distance, and the two are defined as exact
complements: `shellReachIn(d) + SSVeil::reachOut(d) == 1` identically, the same shape the veil law's own
`rimIn + puffRimShare == 1` already takes. That is what keeps the sky from either double-covering or gapping across
the join, and it is one line to pin in a rung. The shell is therefore zero inside 10 km - where the volumetric deck
and its tiled veil own the sky - and carries the deck alone past 14 km.

### Two faces

The shell's shading is the deck's, not a new model, and which face it shows follows the sign of its altitude over the
camera:

- **Below the deck**: the base. `ssveilcore.h`'s law is the shared authority, evaluated at the shell's intersection
  point, so the far band is the same veil continued and the handoff has no tint step. It carries the deck's gloom and
  merges into the haze at the rim.
- **Above the deck**: the tops. The deck's own top shading (`SSDeckShade`) at the intersection, sunlit, so a sky
  build looks down on cloud tops running to a curved horizon instead of onto a hole in the world.
- **Inside the deck**: neither - `plane_fade`'s existing dissolve over 40..300 m hands the sky back to the
  volumetric deck's own volume.

Density comes from the same procedural field the puffs and the veil read (the occupancy/holes chain in
`ssVolCloudF.glsl`), evaluated at the shell's world intersection, so the far sky is genuinely this sky continued
rather than a second texture that happens to sit beyond it. Storm bearing bias (far coupled cells darkening and
thickening the layer at their bearing) rides the same sample and stays deferred to its own step.

### Order and occlusion

Only one configuration puts two shells on one ray: camera below both, looking up, where the deck shell's hit is
nearer than the cirrus band's. Drawing the deck shell after the cirrus band composites that case correctly, and every
other case is disjoint by ray direction (above the deck and below the cirrus, up-rays hit only cirrus and down-rays
only the deck; above both, only the deck). No per-fragment depth comparison between the layers is needed.

### Ladder and cost

Rungs, in the house pattern: a unit rung on the core's intersection against a closed-form reference and on the
tangent elevation (with the flat fallback as the failing control); a rung pinning `shellReachIn + reachOut == 1` over
the join with the pre-8c independent pair as the failing control; a twin holding `ssshellcore.h` against both shader
transliterations; and a scenario sweeping camera height from under the deck, through it, to well above it, asserting
the face flips, that the shell is zero inside 10 km, and that it terminates at the tangent elevation on both faces.

Cost is one extra shell evaluation per sky fragment - one sqrt and a divide - against the annulus mesh's thousands of
quads and its depth-range damage. No new geometry, no new draw call if it rides the dome band's pass.

### What 8c deletes

Three pieces designed earlier in this thread and made unnecessary by the shell: the polar ring tessellation (ruled out
on texture shear), the curvature drop in the deck's vertex stage (at 14 km the drop is 13 m - curvature only matters
past ~50 km, which is the shell's domain and analytic there), and the logarithmic squash fold with its `mEffRadius`
growth (the far band is painted at the far plane, so the depth range is never asked to hold 140 km). Phase 8g's veil
`reachOut` stops being a stand-in for the annulus's inner kilometres and becomes the permanent inner half of the
complement above.

### The superseded mesh design (kept for its reasoning)

A far annulus mesh at deck altitude from `HORIZON_INNER_M` (~8 km) to `HORIZON_OUTER_M` (~200 km), tessellated in
rings so the squash fold and the curvature drop bend it smoothly; shaded from the same de-tiled presence field at the
annulus's air-frame position with thickness faked by a normal from the presence gradient; inner edge crossfading
against Tier B over the 8000/9800 rails, outer edge dissolving into the dome band's melt; drawn in the sky pass at the
squash cap. Rejected on the three findings above. Its one surviving idea is the shading rule - that the far layer must
read the SAME field the puffs do, never a second opinion of it - which the shell keeps.

**Status (2026-09-06): not built.** No `HORIZON_*` or `SSShell` symbol exists. The mid-field patchiness bullet this
section used to carry was a separate fix inside Tier A/B and shipped in 8g; its outcome follows.

### Phase 8g outcome — mid-field patchiness (2026-09-06)

The user's top-down build shot showed the far deck as a regular lattice of identical round blobs behind a stair-stepped tier boundary — the row pattern this section's original prescription was written to fix — but what shipped is not that prescription. `BODY_RADIUS_FRAC` stays at 0.425; the fix is per-BLOCK, not per-sub-puff, and it is a widened crossfade band plus a hashed body jitter and size roll rather than a radius raise: `TIER_BLEND_M` 600 m → `3 * MACRO_M` (1560 m, a per-block weight so the band is measured in macro tiles, not metres), and each Tier B body gets a hashed, deterministic offset from its block centre on a **disc** of radius `0.30 * MACRO_M` (156 m — sampled radially, not per-axis, after a review found the per-axis form could reach past a jittered body's own minimum radius) plus a hashed 0.80–1.25 radius scale, with `BODY_AREA_NORM = 1 / E[s²] = 0.936768` keeping the expected covered area exact despite the per-body variance.

Measured by `tests/deckmacrocore.cpp` and `tests/visual_macro_tier.cpp`: at-centre share 1.000 → 0.026, body radius variance 0 → 826 m² (sd 28.7 m on a mean of 225.7 m), in-blend blocks 69 → 163, mean jitter offset from the block centre 0 → 104.0 m (the disc's exact E[r] = (2/3) × 156 m; a per-axis square would instead average 0.765 × 156 m ≈ 119 m, which is what the first cut of this jitter computed until the review caught it).

## 5b. Cloud and sky light — one airlight, and the dimensionality ladder (proposal, 2026-09-06)

**The request** (user): "I want to make the cloud better match the shading of the sky dome sky but give both some more
dimensionality if possible."

### There is no mismatch to fix - there is a duplication to remove

The deck is not shaded by a model of its own. `ssVolCloudV.glsl` declares the SAME windlight uniforms the sky runs on
(`blue_horizon`, `blue_density`, `haze_density`, `haze_horizon`, `density_multiplier`, `max_y`, `lightnorm`,
`sunlight_color`, `ambient_color`) and composes `vary_ss_sunlit` / `vary_ss_amblit` following, in its own comment,
"cloudsV's exact composition". The airlight ships separately as `additiveColorBelowCloud * (1 - combined_haze)` and is
added AFTER every cloud multiplier, and the reason is recorded at the declaration: folded into the ambient it was
multiplied by the storm gloom and the wrap mids, which stripped the warm dawn air off the deck (cold navy puffs under
a pink cirrus band) and left the deck meeting the horizon band about 35% darker than the band it joins. Added at the
end instead, deck and dome band converge to the SAME pure airlight at the rim - "the handoff is exact by
construction".

So the sky, the cirrus band and the deck already agree by design. What they do NOT share is a single spelling of that
agreement: the composition is transliterated in `cloudsV`, again in `ssVolCloudV`, and again in the sky itself, and
8c's deck shell would make a fourth. **`ssairlightcore.h`** - the beam's extinction to a given altitude, and the
airlight over a path of a given length and direction - with those four as its consumers, is the structural half of
this request. It is worth landing BEFORE 8c rather than after: it is the difference between the horizon shell
inheriting the sky's air and approximating it, and the rim convergence that makes the shell's melt physical rather
than authored depends on exactly that.

### The dimensionality is largely built and switched off

The flow-clouds port (S4) brought three lighting variants into `ssVolCloudF.glsl` behind `SSAtmoCloudLightVariant`,
all dormant because variant 0 is the default and is pinned bit-identical to the pre-port look:

1. **POWDER** - the Beer-powder factor on the direct-sun body term (`research_lighting.md` #1), `d = 2.5 * buried`.
2. **HG2** - a two-lobe Henyey-Greenstein phase on the view/sun angle replacing the `thin^3` rim fringe (#3), tuned so
   the fringe's peak matches the old `SS_RIM` within a couple of percent - the SHAPE changes, not the exposure.
3. **FULL** - both, plus the storm-green tint on buried bellies under gloom (#4's "green sky" cue).

**The first action costs nothing: A/B the variants in a build and pick a default.** This is the largest single
dimensionality change available and it is already written, twin-tested and reviewed.

### What remains, in cost order

`research_lighting.md`'s summary lists six march-free portable blocks; three are unbuilt.

- **Ambient as a sky gradient rather than a scalar** (#4). `vary_ss_amblit` is one flat ambient through the cloud
  colour today. Volume reads when bottoms take the ground-and-haze colour while tops take the zenith. The sky model is
  direction-only and already in scope in that vertex shader, so this is two evaluations - up and down - lerped by the
  puff's height in the layer and its up-ness, both of which the deck already carries (`layer_h`, and the graze ramp
  beside it). This is the literal reading of "match the sky dome's shading", and it is the cheapest real gain.
- **Multi-scatter octaves** (#2). `sum over i of 0.5^i * HG2(cosTheta, 0.8*0.5^i, -0.3*0.5^i) * beer(d)^(0.5^i)`, two
  or three terms, no march. A small extension of the HG2 code already in the file; it supplies the soft interior light
  single scattering cannot.
- **Puff-on-puff self-shadowing** (#6). The research note's own route is to extend an existing top-down shadow bake
  into a 2-4 layer sun-facing deep-opacity stack, re-baked on deck/sun change and sampled once per puff. This fork
  already has `bakeGroundShadow` - a keyed 256^2 transmittance map over the field's extent, with the sun's angle
  applied at SAMPLE time rather than baked in, which is exactly the property a sun-facing stack needs. Biggest win,
  most work.

### The sky's own dimensionality

Two changes, both riding 8c rather than competing with it. **Pivot the sky about the true horizon** using the dip
angle `sqrt(2h/kR)` instead of about eye level, so the haze band, the below-horizon mirror and the horizon clip all
turn about the same line the deck shell terminates on (0.13 degrees at 20 m, 0.94 at 1 km, 1.9 at 4 km). And make
**density altitude-dependent**, so a sky build gets a deeper zenith and a thinner, lower haze band instead of the
sea-level gradient it renders today. The second is the stronger cue and the one that makes altitude read as altitude.

### Order, and the one structural rule

Variants A/B on the next build (free) -> `ssairlightcore.h` and the four-consumer consolidation, before 8c ->
ambient sky gradient -> multi-scatter octaves -> the sky's dip pivot and altitude density, landing with 8c ->
self-shadowing last.

**Every step extends `SSAtmoCloudLightVariant`; none of them changes variant 0.** The pristine look stays pinned by
`twin_lightvariant` over its 58k input combinations, and each addition stays A/B-able against it in a live build -
which is the only place this class of change can actually be judged.

## 6. Phase 8d — debug UI (from the same report)

1. **World dim.** The dim is a 2-D quad drawn *after* the in-world overlay (which renders in `LLPipeline::renderDebug`), so it dims the overlay lines too. Fix: draw the info view's in-world layer from the UI stage, after the dim quad, with the 3-D projection pushed (the HUD-render idiom), depth test off — the world dims, the overlay stays bright, the Skylines look. (An RLV-sphere-style post effect would dim the overlay just the same, since `renderDebug` runs before post-processing.)
2. **Chart beside the legend.** The `ss_atmo_graph_view` widget moves out of the floater's Views tab onto the debug HUD, docked to the right of the legend in the bottom-left; the floater keeps the mode combo and the dim slider.
3. **Tab reorganisation.** Each "Draw the …" checkbox moves into the pane that owns its dropdown: wind flowmap → Wind Flow; rain shadow map and roof runoff → Rain; surface/world/cloud field → Fields; "Show info overlay" → Views. Overlays keeps the markers and the settle marker and is renamed **Markers**. XML-only (controls bind by `control_name`), stretch anchoring with explicit left+right.

4. **The info-view look is a post-screen pass, not a dim** (user, 2026-09-06: "the world dimming still sucks, it is just making the world black at max"). The Skylines look is a warm-gray world with strong ambient occlusion and soft shadowing under a bright overlay. Two ways to get it: replace every world shader with a flat warm-gray variant (more performant per pixel, but this fork has dozens of material paths, and every alpha-blended surface would need its own variant to composite correctly), or a full-screen pass over the finished image that works like extreme fog. Decision: the post-screen pass, at the site the dim quad already owns (after `renderFinalize`, before the HUD). It reads the presented colour, the G-buffer depth and normal, and writes the default framebuffer: luminance of the scene remapped onto a warm-gray ramp (dark warm brown-gray to cream), multiplied by a screen-space occlusion recomputed in the pass from depth and normal (the pipeline's own SSAO buffer is overwritten by the post chain before this point), a soft hemisphere shade from the normal (sky-lit, plus a gentle key from the sun's azimuth so faces read), and a distance fog toward a lighter warm gray beyond a knee so depth reads like the extreme-fog reference; the sky becomes a flat light warm tone. Alpha is handled by construction: blended surfaces are already composited in the colour, and they take the occlusion, shade and fog of the opaque surface behind them, which is what a fog would do too. Cost: one full-screen pass with a dozen depth taps. The dim slider goes; a **Look** toggle replaces it (setting `SSAtmoInfoViewLook`, default on; the overlay's world layer draws on top unchanged, and mode 0 draws neither). Snapshots, cube snapshots and hide-UI skip it like the overlay.
5. **Entity coverage of the views.** Today: V1 wind profile, V2 storm cells + vortices + squall line band + authored pins, V3 deck LOD, V4 virga/shafts (skew polylines), V5 weather cube. The anatomy tier (section 4) is not an extension of V2: it gets its own **V8 Anatomy** view when 8b lands (V6 is World Field, V7 the sync console - the numbering was settled 2026-09-06 and is authoritative in ssatmoinfoviewcore.h, not in any doc), showing each entity's archetype and parts, the operator footprints on the container (displacement, density, emission, shading), the analytic carves, and the container's own depth and density curves at the cursor; the horizon deck (8c) adds a rail to V3. Every entity class that ships gets a view or a layer in one before its phase closes; the rule is written into the phase's review.

## 7. Order

7d (done, this build) → 8d (UI, small) and 8e (shafts) in parallel (disjoint files) → 8a (authored squall + cue + curves: makes the show reachable) → 8b (anatomy) → 8c (horizon deck: the shell core and the cirrus refactor first, then the deck shell and the reach complement - no frame-time gate any more, the shell replaced the mesh whose cost the gate was for).
