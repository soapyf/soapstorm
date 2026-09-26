# Atmo Magic: the backlog — everything decided and not built

Compiled 2026-09-06 from `doc/atmo_magic_*.md`, `V:\Scratch\atmo\PLAN.md`, `V:\Scratch\atmo\BUILD_CHECKLIST.md`,
`V:\Scratch\flow\{BRIEF,RESEARCH,JUDGING,BUILD_CHECKLIST,research_lighting,research_vfx}.md`, the project memory
backlog, and a sweep of `indra/newview` for stubs, stated residuals, unwired settings and dead code.

This is the WORK LEDGER only. The standing rules — determinism, the frame contract, the operator model, the
core/shell split — live in `doc/atmo_magic_architecture.md`. Nothing that is finished appears here.

**Status vocabulary** (fixed; every item carries exactly one):

| status | meaning |
|---|---|
| DESIGNED-NOT-BUILT | specified in a doc, no symbol in the tree |
| PARTIALLY-BUILT | some of it ships, the rest is specified and absent |
| BUILT-BUT-OFF | code ships, a default or a gate keeps it dark |
| KNOWN-DEFECT | measured, understood, deliberately not fixed |
| STUB | a symbol exists that does nothing |
| UNWIRED | written and tested, no consumer calls it |
| DEAD-CODE | defined, never reached |
| OPEN-QUESTION | cannot be settled without a build, a measurement or a decision |
| RESEARCH-ONLY | a technique captured in a research note, never staged |
| UNKNOWN | the sources do not settle it; the item says what would |

---

## 1. Cloud field and deck

**1.1 Lighting variants dormant at the default.** `SSAtmoCloudLightVariant` carries POWDER, HG2 and FULL from the
flow port; variant 0 is the default and is pinned bit-identical to the pre-port look, so all three are dark in
every shipped frame.
*Where:* `BUILD_CHECKLIST.md` §2; `doc/atmo_magic_phase8_show.md` §5b "The dimensionality is largely built and
switched off"; `app_settings/settings.xml:28633`.
*Status:* BUILT-BUT-OFF. *Depends:* every remaining 5b step extends this dial, and the 8c shell's look cannot be
judged against an unsettled baseline. *Size:* small — no code; one build and a verdict.

**1.2 Ambient as a sky gradient rather than a scalar.** `vary_ss_amblit` is one flat ambient through the cloud
colour. Two sky-model evaluations (up and down) lerped by the puff's height in the layer and its up-ness would let
bottoms take the ground-and-haze colour while tops take the zenith.
*Where:* `atmo_magic_phase8_show.md` §5b "What remains, in cost order", item 1; `research_lighting.md` summary #3.
*Status:* DESIGNED-NOT-BUILT. *Depends:* nothing; the inputs (`layer_h`, the graze ramp, the sky model) are already
in scope in `ssVolCloudV.glsl`. *Size:* small — two evaluations and a lerp in a vertex shader that already declares
every uniform it needs.

**1.3 Multi-scatter octaves.** `sum_i 0.5^i * HG2(cosTheta, 0.8*0.5^i, -0.3*0.5^i) * beer(d)^(0.5^i)`, two or three
terms, no march — the soft interior light single scattering cannot supply.
*Where:* `atmo_magic_phase8_show.md` §5b item 2; `research_lighting.md` summary #2.
*Status:* DESIGNED-NOT-BUILT. *Depends:* the HG2 variant code it extends (1.1). *Size:* small — a loop over code
already in `ssVolCloudF.glsl`.

**1.4 Puff-on-puff self-shadowing (deep opacity stack).** Extend the existing `bakeGroundShadow` into a 2–4 layer
sun-facing deep-opacity stack, re-baked on deck/sun change, sampled once per puff. The fork's bake already applies
the sun angle at SAMPLE time rather than baking it in, which is the property the stack needs.
*Where:* `atmo_magic_phase8_show.md` §5b item 3; `research_lighting.md` §6 (Yuksel & Keyser 2008) and summary #5.
*Status:* DESIGNED-NOT-BUILT. *Depends:* nothing structural; it is last in 5b's cost order. *Size:* large — a new
bake target, a re-bake key, a sampling path, and a twin for each.

**1.5 `ssairlightcore.h` has one consumer of six.** The core exists (≈500 lines: `lightAtten`, `extinction`,
`beam`, `beamToElevation`, `glowLight`, `hazeSplit`, `pathTransmittance`, `ambientUnderClouds`, `dimByCloudShadow`,
plus a `Spelling` struct for the per-consumer divergences) and is tested (`tests/unit_airlight.cpp`,
`tests/twin_airlight.cpp`). Only `softClipUnit` is actually consumed, by `ssVolCloudF.glsl`. `cloudsV.glsl`,
`ssVolCloudV.glsl` and the sky still each transliterate the airlight composition independently.
*Where:* `indra/newview/ssairlightcore.h` (untracked); `ssVolCloudF.glsl:840`, `:1882`; design in
`atmo_magic_phase8_show.md` §5b "There is no mismatch to fix — there is a duplication to remove".
*Status:* UNWIRED. *Depends:* **8c depends on this** — the doc is explicit that it lands BEFORE the shell, or the
shell approximates the sky's air instead of inheriting it. *Size:* medium — three call-site rewrites plus their
twins; the hard part (agreeing three spellings) is already solved in the header.

**1.6 Flow S5 — scud shreds and shelf shading.** The wall cloud, tail and shelf were restaged into 8b anatomy
parts and the tornado displacement operator; what survives as S5 is a sparse band of small fast-drifting ragged
puffs under the base, and a layered turbulent underside for the squall line's shelf driven by `ss_line_field`'s
existing shelf term.
*Where:* `V:\Scratch\flow\BUILD_CHECKLIST.md` "Known latents / not yet landed"; `RESEARCH.md` C2.
*Status:* DESIGNED-NOT-BUILT. *Depends:* the shelf half wants 8b-4's line archetype to own the geometry.
*Size:* medium — CPU placement for scud, a fragment layer for the shelf.

**1.7 Flow S6 — erosion carve swap + analytic merge.** Ladder-proven and not implemented. The exponent-share
merge is algebraically exact (0.00000 error) when overlapping quads agree on the kernel sum, and the carve must
land WITH it: the judging round found the candidate carve alone lost the interior texture ("stacked smooth balls").
*Where:* `V:\Scratch\flow\JUDGING.md` rounds 3a, 5b, 6; `BUILD_CHECKLIST.md` "S6 ... designed and ladder-proven
but NOT implemented — the carve/merge pair lands together".
*Status:* DESIGNED-NOT-BUILT. *Depends:* nothing; competes with 8b for the same fragment budget. *Size:* large —
a quad-headroom change (~1.6×), a neighbour-kernel transport scheme, and a re-tuned carve, all twinned.

**1.8 Merge transport, hybrid.** Round 6 settled it: near field carries 2–3 true neighbours on a vertex channel;
far field (Tier B, one body per macro cell) reconstructs neighbours in-shader from the replicated gate hash,
measured exact. Neither half is built.
*Where:* `JUDGING.md` round 6. *Status:* DESIGNED-NOT-BUILT. *Depends:* 1.7 — it is S6's transport half.
*Size:* medium — inside 1.7's estimate; listed separately because the near/far split is a real decision.

**1.9 Billow ring axis is still the view-ray approximation.** The true fix is to build the sphere in the WORLD
frame so its axis stops tracking the camera; this is the same root cause as the wrap bullseye (7.1).
*Where:* `V:\Scratch\flow\BUILD_CHECKLIST.md` "Known latents"; `ssVolCloudF.glsl:1745` names it as review G2 item
(i) and records the billow block as unfixed.
*Status:* KNOWN-DEFECT. *Depends:* fixing it subsumes 7.1's one-liner. *Size:* medium — a frame change that moves
pixels everywhere, so it needs its own build to judge.

**1.10 Tier C impostor cards to 20 km (phase 6e).** The only phase in `PLAN.md`'s table never staged; it was left
pending a frame-time measurement and is still unstaged after 8g.
*Where:* `PLAN.md` §4 table row 6d/6e, §16 "Phase 6e (impostors to 20 km) remains the only unstaged item";
`atmo_magic_far_clouds.md` §4 (stretch goal); `RESEARCH.md` D5 (Harris 2001 re-render amortization).
*Status:* DESIGNED-NOT-BUILT. *Depends:* 8c may make it unnecessary — the shell now carries the far silhouette
past the field, which was the reach argument for impostors. **Decide this before building it.** *Size:* large.

**1.11 Thickness split — the `mBaseDensity` curve.** `mBaseThicknessM` conflates three things: the container's
geometric depth, its optical density, and the height towers grow to. The container model separates them; the
generator authors density, `SSDeckShade::thicknessTerm` takes density × depth, and 8f's "thin before the cue,
thick at the cue" authoring migrates from depth to density.
*Where:* `atmo_magic_phase8_show.md` §4·0 "Thickness is two things"; `PLAN.md` §24.
*Status:* DESIGNED-NOT-BUILT. *Depends:* **8b-1 introduces it** and the anatomy's tower/anvil heights are
expressed against it. *Size:* medium — a new keyframed cube curve, a generator path, a shade-term change, and the
twins that pin the shade term across CPU/GLSL.

**1.12 Storm bearing bias on the far layer.** Distant coupled cells darken and thicken the layer at their bearing,
read at dome-UV resolution rather than as geometry.
*Where:* `atmo_magic_far_clouds.md` §6 (explicitly deferred stretch); `atmo_magic_phase8_show.md` §5 "Two faces"
("stays deferred to its own step").
*Status:* DESIGNED-NOT-BUILT. *Depends:* 8c (2.1) — it rides the shell's own sample. *Size:* small once the shell
exists; the storm layer already exposes bearing and distance.

**1.13 Per-cell, not per-fragment, storm-sample lockstep.** A sub-puff's jitter (up to `CELL_M*0.4`) can put its
fragments over a neighbouring cell's storm sample while the builder shapes the whole sub-puff from one sample.
Carrying the owning cell index on a vertex channel would close it.
*Where:* `ssvolcloud.cpp:1005-1011`; `atmo_magic_storm_dynamics.md:56`.
*Status:* KNOWN-DEFECT (stated, deferred). *Depends:* the spare vertex channel budget, which S6's transport (1.8)
also wants. *Size:* medium — a channel allocation plus a re-read path in the fragment stage.

**1.14 The rotated second lattice cannot be aligned by the drift wrap.** Honest residual of the 6b de-tile.
*Where:* `PLAN.md` §13. *Status:* KNOWN-DEFECT. *Depends:* nothing. *Size:* UNKNOWN — no fix has been designed;
settling it means re-deriving the wrap against both lattices' periods.

**1.15 `RIM_FULL` amplitude at the rim.** The near-field cap of 0.75 was kept deliberately; 0.90 at 9.8–10 km is
the new number and the one most likely to need a tune.
*Where:* `BUILD_CHECKLIST.md` "Open questions this build should answer" #2; `ssVolCloudF.glsl:466`.
*Status:* OPEN-QUESTION. *Depends:* a build; it cannot be settled in the harness. *Size:* small.

---

## 2. Horizon, sky and water

**2.1 Phase 8c — the horizon deck shell.** A second spherical shell at the volumetric deck's own altitude, sampled
per fragment from the view ray, painted in the sky pass at the far plane. Reach is the sum of two tangent lengths
(`sqrt(2kRh) + sqrt(2kR*z_deck)`) — 127 km for a 1000 m deck from the ground, 244 km from a 1000 m sky build. Both
faces (base below, sunlit tops above), density from the same procedural field the puffs read.
*Where:* `atmo_magic_phase8_show.md` §5 (whole section); `PLAN.md` §26.
*Status:* DESIGNED-NOT-BUILT — the doc says so at §5 "Status (2026-09-06): not built", and a sweep confirms no
`HORIZON_*` or `SSShell` symbol exists anywhere in `indra/`. *Depends:* 2.2 and 1.5 both land first.
*Size:* large — a core, a new shader, a refactor of `cloudsF.glsl` onto the same core, and a five-rung ladder.

**2.2 `ssshellcore.h` and the `cloudsF` refactor.** `ss_plane_base` already intersects the true view ray with a
spherical shell, takes altitude as a parameter and handles both faces — but three of its constants are calibrated
to the cirrus band and one case is missing. All four must change before a second shell may call it:
`SS_DOME_TILE_M` (32 km, pinned against the band's 6 km height) becomes a parameter; `SS_PARALLAX_DAMP` (0.125)
becomes an argument and the deck shell runs UNDAMPED or it slides against the world-honest tiled veil at the
handoff; `SS_DETAIL_LO_M`/`HI_M` (100/250 km) get their own values; and `ss_deck_edge_fade` returns 1.0 for
`alt < 0`, so a layer below the camera gets no rim melt and a deck seen from a sky build would end on a hard circle.
*Where:* `atmo_magic_phase8_show.md` §5 "What must be restructured first".
*Status:* DESIGNED-NOT-BUILT. *Depends:* blocks 2.1. *Size:* medium — the intersection, tangent elevation and rim
fade move into a core, and a twin holds the core against both shader transliterations.

**2.3 The reach complement `shellReachIn(d) + SSVeil::reachOut(d) == 1`.** The shell fades in over exactly the
rails the veil fades out on, defined as identical complements so the sky neither double-covers nor gaps at the join.
*Where:* `atmo_magic_phase8_show.md` §5 "The handoff is a complement, not a crossfade".
*Status:* DESIGNED-NOT-BUILT. *Depends:* 2.1. *Size:* small — one rung pins it, with the pre-8c independent pair
as the failing control.

**2.4 The veil's `reachOut` is a placeholder standing in for 8c.** The veil keeps going past the puffs on rails of
its own (10 000 → 14 000 m) explicitly as a stand-in; when the horizon deck lands, `reachOut` stops being a
stand-in and becomes the permanent inner half of 2.3.
*Where:* `ssVolCloudF.glsl:1086` ("PLACEHOLDER, and ssveilcore.h says so at length");
`atmo_magic_far_clouds.md:46`.
*Status:* STUB (a placeholder with a live claim). *Depends:* 2.1/2.3 resolve it. *Size:* small — it is a
re-labelling once the shell exists.

**2.5 Pivot the sky about the true horizon.** Use the dip angle `sqrt(2h/kR)` instead of eye level, so the haze
band, the below-horizon mirror and the horizon clip all turn about the same line the deck shell terminates on
(0.13° at 20 m, 0.94° at 1 km, 1.9° at 4 km).
*Where:* `atmo_magic_phase8_show.md` §5b "The sky's own dimensionality".
*Status:* DESIGNED-NOT-BUILT. *Depends:* lands with 8c so the two agree on one line. *Size:* small — one angle
threaded through three existing sky terms.

**2.6 Altitude-dependent sky density.** A sky build should get a deeper zenith and a thinner, lower haze band
instead of today's sea-level gradient. The doc calls this the stronger of the two sky cues.
*Where:* `atmo_magic_phase8_show.md` §5b. *Status:* DESIGNED-NOT-BUILT. *Depends:* lands with 8c. *Size:* medium
— it changes every sky pixel, so it needs its own A/B.

**2.7 The wedge below the waterline at altitude.** From a 1000 m sky build roughly 22° of the lower sky is
mirrored haze standing in for ground. Nobody has sized it from a real screenshot.
*Where:* `BUILD_CHECKLIST.md` "Open questions this build should answer" #3.
*Status:* OPEN-QUESTION. *Depends:* what 2.1 and 2.5 must actually cover. **Settle it with one screenshot from
1000 m looking down before scoping the shell's lower face.** *Size:* small to measure.

**2.8 `SSFarWater` — the band past `MAX_FAR_CLIP`.** The class is declared, constructed-in-source and never
instantiated; `SSWaterWorld::rebuildFarWater()` is an empty body called once from the rebuild path. True water
beyond the projection far plane needs eye-anchored geometry with a bespoke projection or shader trick.
*Where:* `sswater.h:53`; `sswater.cpp:92` ("Stub — never instantiated yet"); `sswater.cpp:369-373` (empty body);
`atmo_magic_water.md` §"Deferred / known limits"; constraints in the removed far-sea experiment
(memory `far-sea-eye-anchored`).
*Status:* STUB. *Depends:* the constraints memory says a revival restores `SSAtmoDebugFarSeaFreezeEye` first, or
diagnostic screenshots will show the coverage hole and be tuned against. *Size:* large — it is a projection
problem, not a tiling problem, and the last attempt was deleted.

**2.9 Camera-anchored water tiling on large var regions.** The tile circle is anchored to the region centre; a
camera far from it thins coverage in the camera's direction. `region_width/2` in `ring_reach` keeps every
direction covered past the cap, so this is latent.
*Where:* `atmo_magic_water.md` §"Deferred / known limits". *Status:* KNOWN-DEFECT. *Depends:* nothing.
*Size:* medium — needs hysteresis to avoid rebuild churn.

**2.10 Neighbour-region Atmo environments and cross-sim track sharing.** Every tile and every `SSWater` plane
wears the agent's track today.
*Where:* `atmo_magic_water.md:57`. *Status:* DESIGNED-NOT-BUILT. *Size:* large.

**2.11 Fixed-function water renders the extended ring unsquashed.** The far tiles slice exactly as the old `reach`
avoided. Only bites a deliberately broken graphics path.
*Where:* `atmo_magic_water.md` §"Deferred / known limits". *Status:* KNOWN-DEFECT (accepted). *Size:* small.

**2.12 The waterline blend quad.** A quad aligned with the water hiding the water transition — soft blended
gradient matching water colour, lining up with any future vertex-shader waves, splitting the underwater fog.
*Where:* project backlog (memory `backlog`, "Waterline"). *Status:* DESIGNED-NOT-BUILT. *Depends:* nothing.
*Size:* medium.

---

## 3. Storms, entities and the anatomy tier

**3.1 Phase 8b-1 — the anatomy core (`ssanatomycore.h`).** Archetype selection (ordinary Cb, supercell,
squall-line member, towering cumulus), part primitives in cell-local coordinates, closed-form placement, analytic
carve primitives, LOD. A sweep confirms no `ssanatomycore.h`, `SSAnatomy` or equivalent symbol exists.
*Where:* `atmo_magic_phase8_show.md` §4 "Sequence"; `PLAN.md` §24 (the design decision).
*Status:* DESIGNED-NOT-BUILT. *Depends:* 1.11 (thickness split) supplies the metres-above-base measure every part
needs. **Blocks 3.2–3.7, 5.1 and half of the flow backlog.** *Size:* large.

**3.2 The supercell swirl (displacement operator).** `R(p) = rotate(p - c, theta(t) * w(|p-c|/r))`, `theta = omega
* age`, `w` a smooth bump vanishing at the rim. Producer applies it after the hero shift; every observer inverts it
in reverse order.
*Where:* `atmo_magic_phase8_show.md` §4 (original) item 1; §4·0 operator table.
*Status:* DESIGNED-NOT-BUILT. *Depends:* 3.1; and flow's S3 meso swirl striations must share this formula rather
than spell a second one. *Size:* medium — twin plus round-trip tests before any call site.

**3.3 Anvil sheet and overshooting dome.** A plume at the equilibrium level spreading downwind with a ~20° fan,
radius growing and alpha fading to `ANVIL_REACH_M x anvil`, sheet-shaded on the `SSDeckShade` veil form; the dome
on top while `overshoot > 0`. Read-only for the bake.
*Where:* `atmo_magic_phase8_show.md` §4 part 2 and §4 (original) item 2. *Status:* DESIGNED-NOT-BUILT.
*Depends:* 3.1. *Size:* medium.

**3.4 Mammatus lobes.** Worley-cellular lobes hanging under the anvil sheet while `mammatus > 0`; today mammatus
is a CPU waist-flatten only.
*Where:* `atmo_magic_phase8_show.md` §4 part 3; `RESEARCH.md` C1 (fragment-side knuckle mottle, ~350 m scale);
`research_vfx.md` B.5 item 2 (geometry-first, not a normal-map trick, because the silhouette is the point).
*Status:* DESIGNED-NOT-BUILT. *Depends:* 3.3. *Size:* small once the sheet exists.

**3.5 Base structures — shelf, wall cloud, tail cloud, flanking line.** The four parts that make a storm base read
as a storm base; the 7f contact offset already names the point the funnel hangs from.
*Where:* `atmo_magic_phase8_show.md` §4 part 4; `research_vfx.md` B.5 item 1 (Sea of Thieves pattern: real
geometry plus two independently rotating distortion layers, static gradient shading).
*Status:* DESIGNED-NOT-BUILT. *Depends:* 3.1; supersedes flow's S5 fragment/card versions (do not land those).
*Size:* large — four parts, each with its own carve.

**3.6 Tower updraft loop.** Puffs carry a periodic vertical offset up the tower, `z += A * frac(age/T + hash)`,
fading in over the bottom 15% and out over the top 15%. Bounded, closed-form, and never crossing a cell boundary,
so observers need not invert it — which must be stated and pinned.
*Where:* `atmo_magic_phase8_show.md` §4 (original) item 4. *Status:* DESIGNED-NOT-BUILT. *Depends:* 3.1.
*Size:* small.

**3.7 The line archetype (8b-4).** A continuous shelf and wall along the segment replacing the line band's disc
coupling — what makes the wall read as a wall rather than seven discs.
*Where:* `atmo_magic_phase8_show.md` §4 "Sequence" 8b-4. *Status:* DESIGNED-NOT-BUILT. *Depends:* 3.1, 3.5.
*Size:* medium — it retires an existing mechanism, so it needs the old one as a failing control.

**3.8 The tornado as a displacement operator on the deck.** Puffs within the funnel's reach rotate about the axis,
are pulled inward and slightly down, so the lowering and wall cloud are the deck's own fabric drawn into the
funnel's top rather than a separate object hung beneath it.
*Where:* `atmo_magic_phase8_show.md` §4·0 "The tornado, in these terms"; `PLAN.md` §24 lesson 51.
*Status:* DESIGNED-NOT-BUILT. *Depends:* 3.2's inversion machinery. *Size:* medium.

**3.9 Kilometre-wide rain-cloud footprint operators.** A `Cb`'s precipitation core and a plain nimbostratus rain
cell are the same density operator at different scales: an ellipse or a kilometres-wide footprint that raises drive
and coverage and darkens, moving closed-form with its wind.
*Where:* `atmo_magic_phase8_show.md` §4·0 "Rain-cloud entities". *Status:* DESIGNED-NOT-BUILT. *Depends:* the
operator plumbing 3.1 establishes. *Size:* medium.

**3.10 Funnel scroll and edge tricks.** Prototype-proven in the flow harness (`visual_scene_funnel.cpp`,
`scene_funnel_*` renders) and never carried into `ssVortexF.glsl`.
*Where:* `V:\Scratch\flow\BUILD_CHECKLIST.md` "Known latents"; `research_vfx.md` (polar-coordinate scroll,
multi-vortex references). *Status:* DESIGNED-NOT-BUILT. *Depends:* nothing. *Size:* small — the prototype is the
spec.

**3.11 The pinned hero funnel window does not cover every pass.** Touchdown spans birth+0.07..birth+0.26 of life,
so with birth floored at `HERO_FUNNEL_BAND_LO` the pass is on the ground only while closest age ≥ 0.27; the 30 km
spawn cap pushes it below that when `|motion| * lifetime > 111 km` (anvil wind above ~31 m/s at `LIFE_MAX_S`). No
fixture drives it — the ladder tops out at 25 m/s, closest age 0.383. The constant to move is
`HERO_FUNNEL_LEAD_AGE01` (smaller) or `HERO_FUNNEL_BAND_LO` (lower); never the spawn cap.
*Where:* `ssvortexcore.h:244`; `PLAN.md` §22 "7f CLOSED ... Stated latents".
*Status:* KNOWN-DEFECT (stated latent). *Size:* small — one constant, once a windy-day fixture exists to judge it.

**3.12 The meso offset applies to every supercell funnel and no non-hero rung pins it.** The 25–55% radius contact
offset was introduced for the hero path and now governs every supercell funnel in the field, untested off that path.
*Where:* `PLAN.md` §22 "Stated latents". *Status:* KNOWN-DEFECT (test gap). *Size:* small — one rung.

**3.13 An authored pin beyond the coupling reach loses slot preference.** `slotPreferred(flag, reach)` fixed the
30 km cull-exempt hero holding a slot it could not influence; the authored-pin case inherits the same rule and
loses its preference when placed far out.
*Where:* `PLAN.md` §22 "Stated latents". *Status:* KNOWN-DEFECT. *Size:* small.

**3.14 Authored line members are truncatable under cap pressure.** They are emitted inside their own epoch's
block, after earlier epochs' discrete draws, so under `ACTIVE_CAP` pressure they can be truncated where the
authored pin (emitted first) cannot. Unreachable at cap 512 against a severe-day population of 20–40; the fix, if
the cap ever tightens, is to emit the forced line's members before the epoch loop.
*Where:* `ssstormcellcore.h:714`. *Status:* KNOWN-DEFECT (stated latent). *Size:* small.

**3.15 The generator's curve-rebuild window can reach into the other spell's tail.** ~10% of two-spell rolls. The
fix is named in the source of record: clamp `window_lo` to the peak spell's slice start.
*Where:* `PLAN.md` §23 "One low latent recorded". *Status:* KNOWN-DEFECT. *Size:* small — one clamp.

**3.16 Weather Influence severe *strength* is stored, serialised and never read.** The two Allow gates work; the
strength slider beside them drives nothing.
*Where:* `ssfloateratmoinfluence.cpp:108` ("stored and serialised but not yet read by the gate").
*Status:* UNWIRED. *Depends:* nothing. *Size:* small — but it needs a decision about what strength should scale
(population, intensity floor, or both) before it is wired, or it becomes a second uncalibrated dial.

**3.17 Epoch boundaries drift across phases on half the day-length settings.** Epoch boundaries sit at fixed
phases only when the day length is a multiple of `EPOCH_S` (30 min); the day-length slider steps 0.25 h, so on
half its settings a boundary drifts across phases over the day.
*Where:* `atmo_magic_phase8_show.md` §2. *Status:* KNOWN-DEFECT (stated). *Size:* small to snap the slider,
medium to make epochs length-relative.

**3.18 Funnel/puff cross-pass ordering.** `SSVortexRender::render()` runs after every puff for the frame is
submitted, both with ordinary depth testing and no cross-pass sort, so a puff at the same depth drawn moments
earlier can never be occluded BY the funnel in the alpha-blended sense.
*Where:* `ssvortexrender.cpp:518` ("Review finding 10 (ordering residual, accepted)").
*Status:* KNOWN-DEFECT (accepted). *Size:* large to fix properly (a cross-system sort); the soft-depth agreement
keeps the seam soft, which is why it was accepted.

**3.19 The weather-idea catalogue.** Sandstorm, firestorm/firenado, tornado-as-authored-event, hurricane, magnetic
lines, vertex-shader shatter, impact shatter, meteor shower, artillery barrage, static dust storm, methane gales,
pyrocumulus tornadoes, ink rain, ink mists, blood rain, mana auroras, void squalls, razor gales, mercury downpour,
ozone fractures, glass sleet, aetheric ignition front, phosphor gale, sub-zero singularity blizzard, volcanic
eruption, detonations, hydrogen bomb.
*Where:* memory `backlog`, "Weather ideas". *Status:* DESIGNED-NOT-BUILT (idea list, no specs).
*Depends:* most are re-skins of machinery 8b and 3.9 create. *Size:* small each AFTER the anatomy tier; large
before it — which is the argument for not starting any of them yet.

**3.20 Fire whirls.** Explicitly out of scope: not a meteorological vortex, would belong to a fire system.
*Where:* `atmo_magic_storm_dynamics.md:83`. *Status:* DESIGNED-NOT-BUILT (declined).

---

## 4. Precipitation, surface and wind

**4.1 The virga skew chord residual at the magnitude-cap knee.** One card per stack straddles `SKEW_CAP_M`'s knee
and is a chord of a piecewise path rather than two points on a line; its overlap partner, evaluated at its own
zTop/zBot, disagrees across their shared band.
*Where:* `ssvirgacore.h` header contract; `ssvolcloud.cpp:2534`; `atmo_magic_phase8_show.md` §3b item 1.
*Status:* KNOWN-DEFECT (stated residual). *Size:* small — evaluate the shared band from one side, or split the
straddling card at the knee.

**4.2 `keepHash`'s "keeps everything when n <= cap" invariant is exact only under a stated condition.**
*Where:* `ssvirgacore.h:318`. *Status:* KNOWN-DEFECT (stated residual). *Size:* small.

**4.3 A client whose precip LOD dial zeroes the `TIER_SHEETS` radius sees no shafts at all.** Per-client on/off by
the F6 guard.
*Where:* `PLAN.md` §14 "Residual (stated in code)". *Status:* KNOWN-DEFECT. *Size:* small — decouple the shaft
gate from the particle tier's radius.

**4.4 The far squash fold still affects a curtain's mid-air cards against tall terrain.** The ground lift keeps
ground-level cards out of the fold; mid-air cards are unprotected, as far puffs are.
*Where:* `ssvirgacore.h` header; `ssvolcloud.cpp:1516`. *Status:* KNOWN-DEFECT (accepted, shared with the deck).
*Size:* large — it is the squash fold's own problem, which 8c's shell sidesteps rather than solves.

**4.5 WorldField — the unbuilt half.** Still absent: the wind solve's interior-skip (part of migration step 3,
gated on multi-peel spans), wind capture absorption, WALKABLE (7), ACOUSTIC (8), and Design H (6).
*Where:* `atmo_magic_worldfield.md` implementation-status block, lines 17–21; memory `backlog` ("WorldField").
*Status:* PARTIALLY-BUILT. *Depends:* the interior-skip is gated on multi-peel spans (`SOLID_VOLUME_3D`).
*Size:* large — WALKABLE alone is a navmesh.

**4.6 The two WorldField consumers ship default-off.** The wet field reads `surfaceTop` behind
`SSWorldFieldSurfaceTop` (default off) and the soundscape's cover/burial read the COVERAGE channel behind
`SSWorldFieldCoverage` (default off) — so the capture service runs and almost nothing consumes it in a default
session.
*Where:* `atmo_magic_worldfield.md` status block; `app_settings/settings.xml:27027`, `:27038`.
*Status:* BUILT-BUT-OFF. *Depends:* a build verdict on each. *Size:* small — flipping them is free; judging them
is a build.

**4.7 Snow: the static per-slab deposit-potential field.** Divergence-weighted calm zones per slab, solved once
with the flowmap, no per-frame anything — the sanctioned next step if the drift band ever reads wrong. The running
density volume stays rejected.
*Where:* `atmo_magic_snow.md` §11 item 6 and §"Deferred / known limits". *Status:* DESIGNED-NOT-BUILT.
*Size:* medium.

**4.8 Snow: the drift band is a band, not a volume.** The air between band top and cloud deck carries no density,
so a gust front does not sweep one street as a visible fog wave while the next stays clear.
*Where:* `atmo_magic_snow.md` §"Deferred / known limits". *Status:* KNOWN-DEFECT (accepted by design).
*Depends:* 4.7 is the sanctioned mitigation. *Size:* large if ever attacked directly.

**4.9 Snow: POM in a deferred pass shades, never silhouettes.** Geometric burial needs surface geometry to bite
into; the fix was considered and rejected.
*Where:* `atmo_magic_snow.md` §"Deferred / known limits"; `atmo_magic_snow_review.md:126`.
*Status:* KNOWN-DEFECT (accepted). *Size:* large (declined).

**4.10 Snow: cross-region drift is not shared at borders.** Wind stays continuous through the flowmap's margin
overlap and particles cross fine; the accumulated bank at a border is not shared state.
*Where:* `atmo_magic_snow.md` §"Deferred / known limits". *Status:* KNOWN-DEFECT (accepted). *Size:* large.

**4.11 Snow: the near-camera streak layer is gated on the coarse camera cell.** Stand at a courtyard's edge and a
few streaks appear where the air is still. The fix is named: use the precise `sample()` there, one call per frame.
*Where:* `atmo_magic_snow.md` §12. *Status:* KNOWN-DEFECT. *Size:* small — one named call swap.

**4.12 Thermal mirage distortion.** A full screen-space specification exists as a research document; no `mirage`,
`Thermal*` or equivalent symbol exists anywhere in `indra/newview` on this branch. (The `feature-surface-weather`
branch has a `ssscreenfxcore.h` covering thermal shock and mirage — that is a different branch.)
*Where:* `doc/atmo_magic_thermal_distortion.md` (the doc itself flags "code should be rewritten/adapted").
*Status:* RESEARCH-ONLY on `feature-atmo-magic`. *Size:* medium.

**4.13 The whiteout is obsolete and its replacement lives on another branch.** The user's verdict is that the old
whiteout is "terrible/obsolete" and the fog must be a from-scratch post-process screen-space layer
(`ssheightfog.*`, `ssPostFogF.glsl`, drawn at the start of `renderFinalize`). `sswhiteout.cpp` and
`ssWhiteoutF.glsl` still ship here.
*Where:* memory `surface-weather-feature`. *Status:* KNOWN-DEFECT (superseded on this branch, replaced on
`feature-surface-weather`). *Depends:* the merge of that branch. *Size:* medium — the removal, not the
replacement.

**4.14 Surface weather phase 3.** Height fog (rename + sources), the screen-FX singleton and its two post passes,
wiring, twins, opus review, fix — never run. Phase 2 is fixed and awaiting the user's build.
*Where:* `V:\Scratch\surface\PLAN.md` phase table and "Phase 2 outcome ... awaiting the user's build".
*Status:* DESIGNED-NOT-BUILT (separate branch). *Depends:* the phase-2 build verdict. *Size:* large.

**4.15 Field accuracy ceiling: the rain-shadow texel cloud goes through walls.** Recorded follow-ups: a near-field
finer window, a WorldField occupancy channel, and per-region long-lived analysis via the region object cache.
*Where:* memory `surface-weather-feature` ("Field accuracy in general (goes through walls) is a known ceiling").
*Status:* OPEN-QUESTION (three candidate routes, none chosen). *Size:* medium to large depending on route.

**4.16 Wind particles.** Particles carried by wind for different biomes/conditions or authored experiences (dust,
ash, sand), with condition-driven behaviour — gale-force or tropical storm throwing things around.
*Where:* memory `backlog`. *Status:* DESIGNED-NOT-BUILT. *Depends:* the custom particle system (4.18) would carry
it. *Size:* medium.

**4.17 Freezing / ice system.** Puddles freeze first after sustained cold; cold enough (or a forced checkbox) and
the water plane(s) / voidwater freeze.
*Where:* memory `backlog`. *Status:* DESIGNED-NOT-BUILT. *Depends:* `feature-surface-weather`'s
`sssurfacestatecore.h` already carries ice/frost cell state — build it there, not here. *Size:* medium.

**4.18 Custom particle system.** *Where:* memory `backlog`. *Status:* DESIGNED-NOT-BUILT. *Size:* large.

**4.19 Soundscape engine improvements.** *Where:* memory `backlog`. *Status:* DESIGNED-NOT-BUILT. *Size:* medium.

**4.20 Wetness shader and puddles review.** Listed as "review implementations for improvements"; the user's later
verdict was that wet and puddle are "passable", which lowers its priority but does not close it.
*Where:* memory `backlog`; memory `surface-weather-feature`. *Status:* OPEN-QUESTION. *Size:* small.

---

## 5. Debug views, UI and tooling

The standing rule (`atmo_magic_phase8_show.md` §6 item 5): *every entity class that ships gets a view or a layer in
one before its phase closes.* Measured against that rule today:

**5.1 V8 Anatomy.** The anatomy tier is explicitly NOT an extension of V2. It needs its own view showing each
entity's archetype and parts, the operator footprints on the container (displacement, density, emission, shading),
the analytic carves, and the container's own depth and density curves at the cursor.
*Where:* `atmo_magic_phase8_show.md` §6 item 5. *Status:* DESIGNED-NOT-BUILT. *Depends:* 3.1; **8b cannot close
without it.** *Size:* medium.

**5.2 The V3 horizon-deck rail.** 8c adds a rail to the deck-LOD view.
*Where:* `atmo_magic_phase8_show.md` §6 item 5. *Status:* DESIGNED-NOT-BUILT. *Depends:* 2.1; **8c cannot close
without it.** *Size:* small.

**5.3 V4 has no chart panel.** `SSAtmoGraphView::draw()` draws nothing at all for `MODE_PRECIP_VIRGA` — it falls
through the same switch that dispatches the other four. The design specifies presence/tower sampled along a
camera-forward transect.
*Where:* `ssatmoinfoview.h:423` (the widget's own comment names V4 as the mode with no chart);
`atmo_magic_debug_views.md` §2 V4. *Status:* PARTIALLY-BUILT (in-world layer ships, chart does not).
*Depends:* nothing — the data is already resident in the virga info-view core. *Size:* small.

**5.4 Lightning has no view or layer.** `SSLightning`/`SSLightningRender` is a shipped entity class with strike
geometry, buckets and gates; there is no info-view mode for it and no `RENDER_DEBUG_*` overlay bound in
`ssfloateratmodebug.cpp` (the seven bound masks are wind flow, rain shadow, roof runoff, surface field, world
field, cloud field, geom settle).
*Where:* `ssfloateratmodebug.cpp:41-47`; rule at `atmo_magic_phase8_show.md` §6 item 5.
*Status:* DESIGNED-NOT-BUILT (rule violation, pre-dating the rule). *Size:* medium — strike paths, charge state
and the gate readout are the obvious content.

**5.5 The V6 numbering collides.** `atmo_magic_debug_views.md` §2 assigns V6 to World Field (existing, restyled);
`atmo_magic_phase8_show.md` §6 item 5 assigns V6 to Anatomy. The World Field overlay is not an info-view mode at
all today — it is `RENDER_DEBUG_WORLD_FIELD`, a checkbox.
*Status:* OPEN-QUESTION. **Settle the numbering before 8b writes `MODE_ANATOMY`.** *Size:* small.

**5.6 World Field restyle under the shared legend and palette.** The catalogue promises it; the overlay still runs
on its own colours outside the info-view framework.
*Where:* `atmo_magic_debug_views.md` §2 V6, §4 item 5. *Status:* DESIGNED-NOT-BUILT. *Size:* medium.

**5.7 A UI snapshot keeps the legend but not the overlay.** Cosmetic, left open by the orchestrator.
*Where:* `PLAN.md` §24 ("Left open, cosmetic: N4"). *Status:* KNOWN-DEFECT. *Size:* small.

**5.8 The legend's `default:` branch prints "not implemented yet".** The honest fallback for any mode the switch
does not handle — correct behaviour, and the marker that will fire the moment V6 is added to the combo before its
spec builder exists.
*Where:* `ssatmoinfoview.cpp:2241`. *Status:* STUB (deliberate). *Size:* none — listed so it is not mistaken for
a bug when V6 lands.

**5.9 The `Planetary` panel is a 122-line stub ranked equal to the 1038-line Weather panel.** The environment
editor's information architecture doc records the re-cut; the ranking problem is stated as a symptom of the tab
order, not fixed.
*Where:* `atmo_magic_env_ui.md:7`. *Status:* PARTIALLY-BUILT. *Size:* medium.

**5.10 Generalising `mCloudField` / `mUnderField` into a vector of decks.** Deferred; two named decks ship. The
Add Deck / Remove Deck rail reads as generic but bottoms out on the pair.
*Where:* `atmo_magic_env_ui.md:516-519`. *Status:* DESIGNED-NOT-BUILT. *Size:* large.

**5.11 `SSAtmoDimView` is vestigial.** The class kept only so `LLDebugView::addChildInBack` and the child order are
untouched; it draws nothing, and no `SSAtmoInfoViewDim` setting exists.
*Where:* `ssatmoinfoview.h:364`. *Status:* DEAD-CODE (deliberate). *Size:* small to remove, and the comment
explains why it was not.

**5.12 The `ss_atmo_graph_view` XUI tag is still registered.** The debug HUD's instance is built programmatically
by `SSAtmoInfoView::attach`; the tag survives "for any future XUI use".
*Where:* `ssatmoinfoview.h:423`. *Status:* DEAD-CODE (deliberate). *Size:* small.

**5.13 Tooling: `visual_flow_analytic` fails its own hypothesis check.** Red in `--changed` runs, left to its
owner; the flow harness moved to `V:\Scratch\flow` and this test is the prototype ladder for S6's merge.
*Where:* `PLAN.md` §23 ("Unrelated red in --changed ... left to its owner").
*Status:* KNOWN-DEFECT (harness). *Depends:* 1.7. *Size:* small.

---

## 6. Performance and infrastructure

**6.1 Multicore offload.** The backlog names it generally; `perf_opportunities.md` item 6 names the concrete first
target — offload skinning/rigging math (`updateRiggedVolume`, skinning palettes; pure math, no GL) using the
`SSWindFlowMap::postWorker` pattern (postTo General plus a generation counter).
*Where:* memory `backlog`; `doc/viewer/perf_opportunities.md` §"Main thread" item 6.
*Status:* DESIGNED-NOT-BUILT. *Depends:* nothing — the pattern already ships. *Size:* medium.
*Caution:* `RESEARCH.md` D3 records the house rule — no worker threads in Atmo, and GL workers are known
driver-fault territory (memory `windflow-workergl-nvidia-fault`).

**6.2 `SSAtmoWindFlowWorkerGL` ships default OFF after an NVIDIA bind crash.** Diagnosed as a driver fault; three
latent windflow bugs were found alongside and are recorded in memory.
*Where:* `app_settings/settings.xml:27335`; memory `windflow-workergl-nvidia-fault`.
*Status:* BUILT-BUT-OFF plus KNOWN-DEFECT. *Size:* UNKNOWN — the crash is not ours to fix; the three latents are
small each. **Read that memory before re-enabling.**

**6.3 `perf_opportunities.md` items identified and not taken.** Async GL uploads / PBO streaming (item 2); VRAM
accounting ~2× undercount steering the discard bias (item 3, `llviewertexture.cpp:526`); consolidating the two
one-file-per-asset caches into a packed store (item 4); async `LLFileSystem` reads — fast-cache loads are blocking
on the MAIN thread with an acknowledged multi-second-stall comment (item 5); occlusion-query readback never
blocking (item 7); budgeting `gIdleCallbacks` and texture loaded-callbacks, both unbounded per frame (item 8); UDP
message decode's three copies (item 9); login inventory gunzip+LLSD on main (item 10); unifying `reset_login()` and
`disconnectViewer()` (item 11).
*Where:* `doc/viewer/perf_opportunities.md`. *Status:* DESIGNED-NOT-BUILT (nine items).
*Size:* mixed — items 7 and 8 are small; item 4 is large.

**6.4 `perf_opportunities.md` simplifications not taken.** The duplicated probe-occlusion block
(`pipeline.cpp:2817-2856`, a verbatim copy for hero probes); the `setSkipRenderFlag` asymmetry between the deferred
and post-deferred loops (`pipeline.cpp:4349` vs `:4503`); the dead UDP texture-fetch path; the compiled-out
`ENABLE_GL_WORK_QUEUE` VBO worker.
*Where:* `doc/viewer/perf_opportunities.md` §"Simplifications". *Status:* DEAD-CODE / duplication.
*Size:* small each.

**6.5 The Electron/Tauri UI overlay path.** Verdict recorded as plausible as an overlay, not a replacement; the
missing piece is a websocket↔LLEventPump shim (no websocket code exists in-tree), and the incremental path is one
floater over LLLeap.
*Where:* `doc/viewer/perf_opportunities.md` §"UI". *Status:* RESEARCH-ONLY. *Size:* large.

**6.6 The full-suite harness gate is still owed.** The user's run of `node V:\Scratch\atmo\test.js` covering 7e,
7f, 8e, 8e-c, 8a, 8f and 8g together. Agents have been running impacted sets (`--changed --exclude flow`)
throughout.
*Where:* `PLAN.md` §21, §22, §23 ("Full-suite gate: the user's").
*Status:* OPEN-QUESTION (process). *Depends:* **everything downstream assumes it passes.** *Size:* small to run.

**6.7 Shell code the harness cannot compile is verified only by the user's build.** Named instance: the
`phaseAt`/`wallTimeAtPhase` fix reading captured `mDayLengthS`/`mDayOffsetS` lives in `ssstormcells.cpp`; the core
it calls is pinned, the shell edit is not.
*Where:* `PLAN.md` §16 ("Honest residual"). *Status:* OPEN-QUESTION (coverage gap, structural).
*Size:* large to close in general (it would mean compiling shell code in the harness); small per instance.

**6.8 Region Landscape Cache.** Aggressive cache of a region's long-term static objects plus sim knowledge (water
height, terrain textures, heightmap), ghosted on connect and consolidated against reality over the first minutes
via the LSL Bridge's `llKey2Name` batch verify and `llGetObjectDetails` rez time; trash the region's cache after a
~1–2 minute timeout if the error rate exceeds ~30%.
*Where:* memory `backlog`; design in `doc/region_object_cache.md` and `doc/roc_bridge_spec.md`; `ssroc*.cpp`
ships parts of it.
*Status:* PARTIALLY-BUILT. *Size:* large.

**6.9 The rest of the project backlog, unstarted.** Touch Pointer Capture; material classification of static
region builds; destructibility and effects; decorative damage (bullet holes, chip damage); system interactions and
emergent consequences; reset forced environment on teleport (but never on region crossing, and never persisted
across sessions); fix aim/mouselook up; full vertical sim orientation support; alt-cam linkset motion desync;
make void water alt-cam'able; alt-cam far clip distance; HUD alpha blending anti-aliasing; temp attachment
ghosting after region crossings; inventory perf/caching/sorting; changing material wipes UV coords; object quality
heatmap; context-menu copy URI for images; mesh uploader should respect object size (issue #1); particles on child
prims not interpolated.
*Where:* memory `backlog`. *Status:* DESIGNED-NOT-BUILT (nineteen items). *Size:* mixed; several (teleport
environment reset, copy URI, material UV wipe) are small.

---

## 7. Known defects deliberately not fixed

The five measured-and-left findings from `BUILD_CHECKLIST.md` §11 first, then review findings recorded as accepted.

**7.1 The wrap "bullseye" on every card.** `ss_sphere_normal` builds its fake sphere around the *view ray*, so the
pole tracks the camera; looking up at a deck lit from above, the pole is every card's anti-light point. A puff's
core loses **21.5%** of its radiance between a 5° and an 89° view (1.546 → 1.213) while nothing behind it dims, and
the ring gain rises 1.209 → 1.277. The one-line candidate — ease `wrap` toward 0.5 by
`smoothstep(0.55, 0.95, abs(dot(nrm, ss_light_dir)))`, the identity for every side-on puff — holds the core flat
(1.546 → 1.578) and is quoted verbatim in the shader. It was written, measured and **reverted on purpose**: two
unrequested changes to the same pixels on one build cannot be judged apart.
*Where:* `ssVolCloudF.glsl:1737-1747`; `tests/unit_deck_radiance.cpp`; `BUILD_CHECKLIST.md` §11 item 1.
*Status:* KNOWN-DEFECT (fix ready). *Depends:* 1.9 is the fuller answer and subsumes it. *Size:* small.

**7.2 The soft clip will make that ring visible.** At bright daylight and 89° the old clamp gave core and annulus
both exactly 2.0000 (ring gain 1.0000); the curve gives 1.8408 and 1.9022 (gain 1.0333). It reveals, it does not
amplify.
*Where:* `BUILD_CHECKLIST.md` §11 item 2. *Status:* KNOWN-DEFECT (consequence of a landed fix).
*Depends:* if rings appear in the next build, 7.1 is the answer and it is ready. *Size:* none — it is 7.1.

**7.3 The veil's sphere normal faces the wrong way.** It shades as an up-facing surface (`+Z`) although the sheet
is wound to face down. Not flipped, because the one-token change cuts the daylight veil by up to 45%
(wrap 1 → `SS_FORM_DARK`) on the same build as the gloom fix. Neither ±Z is really right: a cloud base is lit by
the whole lower sky.
*Where:* `ssVolCloudF.glsl:1103-1108`; `BUILD_CHECKLIST.md` §11 item 3.
*Status:* KNOWN-DEFECT. *Depends:* 1.2 (a two-point sky ambient) is the honest fix — the veil then takes the
downward colour instead of a sign. *Size:* small as a flip, medium as the right answer.

**7.4 The deck is ambient-dominated — a second, independent cause of daylight flatness.**
`SSDeckShade::shade` crushes the form term exponentially — about **0.033** for a mid-column puff in a 400 m deck at
sun elevation 0.3 — so the direct-sun body term is only a few percent of a puff's light. That is why POWDER's 92%
cut is small *even before* the clip was involved.
*Where:* `BUILD_CHECKLIST.md` §11 item 4 ("probably the biggest remaining lever on 'the clouds don't match the
sky'"). *Status:* KNOWN-DEFECT. *Depends:* it changes the meaning of every lighting variant, so it should be
calibrated BEFORE or WITH 1.1's verdict, not after. *Size:* medium — a calibration pass with the existing
radiance rung as its instrument.

**7.5 The veil's noise is structurally mid-grey.** Its 5-way Penrose read averages the map's variance out, so it
can never reach the bright end a puff's contrast-stretched noise reaches (~15% short at the top).
*Where:* `BUILD_CHECKLIST.md` §11 item 5. *Status:* KNOWN-DEFECT. *Size:* medium — either fewer taps with a
contrast stretch, or accept it as the veil's signature.

**7.6 The camera's altitude band selects which track's storm world is observed.** Accepted with an honest comment
after the phase-2 determinism audit: the altitude band selects the track exactly as it selects the sky, and each
track's world is a pure function of that track's cube.
*Where:* `PLAN.md` §8. *Status:* KNOWN-DEFECT (accepted by design). *Size:* n/a.

**7.7 Phase 4c's three stated residuals.** Per-cell (not per-fragment) lockstep — see 1.13; fragment-z vs puff-z
shear within a puff (up to ~250 m near the anvil at `SHEAR_LEAN_S = 600`); and the deck base's
`SSAtmoCloudSeason` dependence, shared with the deck itself and stated at the call site.
*Where:* `PLAN.md` §10; `ssdeckframecore.h:34` ("Residuals, stated"). *Status:* KNOWN-DEFECT (accepted).
*Size:* medium each.

**7.8 The card frame's near-degenerate twist.** `ref` sweeps +Z → +X across the same band the normal sweeps toward
±Z, and the two cross near `|n.z| ≈ 0.725` for a puff whose horizontal bearing is along +X (cross length 0.0087 at
46.25° elevation). The guard never fires — 0.0087 is well above `FRAME_EPS` — but the card's orientation twists
through most of a half turn over a couple of degrees of elevation for that one bearing. A property of the shipped
card geometry; moving it rotates cards on screen.
*Where:* `ssdeckflowcore.h:70-82`; `tests/unit_deckflow.cpp` `frame_near_degenerate`.
*Status:* KNOWN-DEFECT (stated, measured). *Size:* medium — it is a visible change and needs its own build.

---

## 8. Unwired settings and dead code

**8.1 There are no orphan `SSAtmo*` settings.** All 192 `SSAtmo*` keys in `app_settings/settings.xml` resolve to at
least one reference in a `.cpp`/`.h`. The single name with no key of its own is `SSAtmoImpactSounds`, which appears
only inside `SSAtmoSounds`'s Comment string (`settings.xml:26535`) — a stale prose reference to a setting that does
not exist, not an unwired control.
*Status:* n/a — recorded because "unwired settings" was expected to be a populated category and is not. The real
gating problem is defaults, below.

**8.2 Settings that ship gated dark.** `SSAtmoCloudLightVariant` = 0 (1.1); `SSAtmoInfoViewTileTint` = off
(deliberate, per the 8g verdict — the cell is the entity's scheduler, the lattice only seeds it);
`SSAtmoWindFlowWorkerGL` = off (6.2); `SSWorldFieldSurfaceTop` and `SSWorldFieldCoverage` = off (4.6).
*Status:* BUILT-BUT-OFF (five). *Size:* small to flip, one build each to judge.

**8.3 `ssairlightcore.h` — see 1.5.** *Status:* UNWIRED.

**8.4 Weather Influence severe strength — see 3.16.** *Status:* UNWIRED.

**8.5 `SSFarWater` and `rebuildFarWater()` — see 2.8.** *Status:* STUB.

**8.6 `LLVOWLSky::drawFsSky`.** Declared `// fullscreen sky for advanced atmo` and defined at
`llvowlsky.cpp:296`; the only two references in the whole tree are that definition and its declaration
(`llvowlsky.h:55`). No draw pool calls it.
*Status:* DEAD-CODE. *Size:* small — delete, or wire it if 8c wants a full-screen sky path (worth checking against
2.1's "no new draw call if it rides the dome band's pass" before deleting).

**8.7 `SSAtmoDimView` — see 5.11.** *Status:* DEAD-CODE (deliberate).

**8.8 The `ss_atmo_graph_view` XUI registration — see 5.12.** *Status:* DEAD-CODE (deliberate).

**8.9 Stale doc claims contradicted by the tree.** `atmo_magic_snow.md:31` says "snow is stored but not yet
shaded — no surface pass reads the snow channel today", but `ssSurfaceSnowF.glsl` ships and `ssgranular.cpp` ships
the granular transport. The doc's status table pre-dates the snow build.
*Status:* KNOWN-DEFECT (documentation). *Size:* small — one table row.

---

## 9. Research captured but never applied

**9.1 `research_lighting.md` portable blocks.** Of six march-free blocks, three are unbuilt: #3 the ambient sky
gradient (→ 1.2), #2 the multi-scatter octave loop (→ 1.3), #5 the baked per-puff directional occlusion lobe (Sea
of Thieves pattern — bake once, evaluate against live light direction plus Lambert at runtime, no march ever), and
#6 the deep-opacity stack (→ 1.4). #4's green-storm tint exists only inside the FULL variant (1.1). Block #5 has no
ledger item elsewhere and is the one genuinely unclaimed idea in that note.
*Status:* RESEARCH-ONLY. *Size:* #5 is medium — a bake, a vertex channel, and a runtime evaluation.

**9.2 `research_vfx.md` Topic B synthesis.** Three recipes with no numeric parameters in the source (they would
have to be originated): the whale's-mouth / shelf-cloud roll as real geometry with two independently rotating
distortion layers and static-gradient shading; mammatus as geometry-first lobes (→ 3.4); asperitas as an
undulatus underside displaced by an extra large-scale low-frequency domain warp. Asperitas has no ledger item
anywhere else.
*Status:* RESEARCH-ONLY. *Size:* asperitas is small on top of 3.5's underside work.

**9.3 `RESEARCH.md` families not staged.** B3 the crowding scalar (per-puff neighbour count widening the window
and flattening the rim on the crowded side); B4 spherical billboards (Umenhoffer 2006 — per-pixel path length
through the implied sphere instead of a flat window; better thickness cue, kills hard intersections, pairs with
B3); C1's fragment-side anvil knuckle mottle (~350 m scale, shader-only); C2's visual wall-cloud lowering (extend
puff bodies below `ss_base_z` where meso is high, fragment-gated, cosmetic — a cheaper stand-in for 3.8 if 3.8
slips); D1 vertex-stage offload of per-corner flow vectors and storm samples; D3 bake-key LUTs for the polar
billow and anvil outflow. D4 six-way lighting is explicitly SKIPPED.
*Status:* RESEARCH-ONLY. *Size:* B4 is medium and would touch every puff; the rest are small each.

**9.4 The funnel reference library has a hole.** Group C yielded only 4 funnel keepers after a Commons rate limit
traced to orphaned background fetch scripts; 10 pre-verified candidates are listed in `GROUP_C_CAPTIONS.md` for a
later fetch-only pass.
*Where:* `V:\Scratch\flow\JUDGING.md` round 5-scene. *Status:* RESEARCH-ONLY (blocked on a re-fetch).
*Depends:* judging 3.10 and 3.8 against photographs. *Size:* small.

**9.5 The thermal mirage specification — see 4.12.** *Status:* RESEARCH-ONLY.

**9.6 Deep Opacity Maps (Yuksel & Keyser 2008) — see 1.4.** Recorded here because the citation, the 3-layer
result and the "warp the layer spacing to follow the depth map" insight are the design, and they exist nowhere in
the viewer tree.
*Status:* RESEARCH-ONLY.

---

## The critical path

What blocks what, in order. Each arrow is a real dependency stated in the sources, not a preference.

1. **The user's full-suite run and the next viewer build** (6.6). Every closure since 7e is provisional until it
   passes, and three of the four questions below can only be answered in a live build.
2. **Pick the lighting variant default** (1.1, `BUILD_CHECKLIST` open question 1). This is the baseline every
   subsequent look change is A/B'd against; the doc says everything in the 8c/5b plan is easier to judge once it is
   settled. Free.
3. **Calibrate the form crush** (7.4) — because at a form term of 0.033 the variant dial is measuring almost
   nothing, so step 2's verdict is partly an artefact of this. These two are one decision, not two.
4. **`ssairlightcore.h`'s four-consumer consolidation** (1.5) → **before 8c**. Stated explicitly: it is the
   difference between the horizon shell inheriting the sky's air and approximating it, and the rim convergence that
   makes the shell's melt physical depends on exactly that.
5. **`ssshellcore.h` plus the `cloudsF` refactor and the four parameterisations** (2.2) → **8c's deck shell**
   (2.1) → **the reach complement** (2.3) → retires the veil placeholder (2.4), unblocks the V3 rail (5.2) and the
   storm bearing bias (1.12), and shrinks the below-waterline wedge (2.7). The sky's dip pivot (2.5) and altitude
   density (2.6) land with it so the sky and the shell terminate on one line.
6. **The thickness split** (1.11) → **8b-1, the anatomy core** (3.1). Parts are measured in metres above the base
   and the wall darkens by density; without the split, 8b-1 re-encodes the conflation it exists to remove.
7. **8b-1** → 8b-2 (tower + anvil + dome, 3.3) → 8b-3 (mammatus, shelf, wall, tail, flanking line, 3.4/3.5) →
   8b-4 (the line archetype, 3.7). Also gated on 8b-1: the swirl operator (3.2), the tower loop (3.6), the tornado
   displacement operator (3.8), the rain-cloud footprint operators (3.9), flow's surviving S5 items (1.6), and
   **V8 Anatomy** (5.1), without which 8b cannot close under its own rule.
8. **Settle the V6 numbering collision** (5.5) before 8b writes a mode constant.
9. **Flow S6** (1.7 + 1.8) is independent of 8b in dependency terms but competes with it for the fragment budget
   and the spare vertex channel — and 1.13's fix wants that same channel. Sequence them; do not run both.
10. **Tier C impostors** (1.10) should be **re-decided, not scheduled**: 8c's shell carries the far silhouette
    past the field, which was the reach argument for impostors in the first place.

Nothing on this path depends on any item in areas 4, 6 or 9.

---

## Highest value for least work — top ten, ranked

1. **A/B the lighting variants and set the default** (1.1). Zero code, already written, twin-tested and reviewed
   over 58k input combinations; it is the largest single dimensionality change available and it unblocks the
   judgement of every other look item.
2. **Calibrate the form crush** (7.4). The measurement is already done and the number (0.033) explains why the
   deck reads flat in daylight — this is the fork's own verdict on the biggest remaining lever on "the clouds don't
   match the sky", and item 1 is partly meaningless without it.
3. **Land the wrap bullseye one-liner** (7.1). Written, measured, quoted verbatim in the shader, and reverted only
   for build hygiene; it recovers a fifth of a puff's core radiance for one `smoothstep`.
4. **Ambient as a two-point sky gradient** (1.2). The literal reading of the user's request, two evaluations in a
   vertex shader that already declares every uniform and already carries `layer_h` and the graze ramp.
5. **Clamp the generator's rebuild window to the peak spell's slice start** (3.15). One clamp; it fixes ~10% of
   two-spell severe rolls silently corrupting the other spell's tail.
6. **Give V4 its chart** (5.3). The only shipped info view missing its panel, the graph widget already dispatches
   on mode, and the virga info-view core already holds the data — it is a `case` and a polyline.
7. **Wire `ssairlightcore.h`'s remaining consumers** (1.5). Roughly 500 lines of core plus its unit and twin tests
   are already green and doing nothing; only the three call sites remain, and 8c is blocked behind them.
8. **The sky's dip pivot** (2.5). One angle, `sqrt(2h/kR)`, threaded through three sky terms that already exist,
   turning the haze band, the mirror and the horizon clip about the line the deck actually terminates on.
9. **Multi-scatter octaves** (1.3). A two-or-three-term loop over the HG2 code already sitting in
   `ssVolCloudF.glsl`, supplying the soft interior light single scattering cannot — and it extends the dial rather
   than changing variant 0.
10. **Build the billow's ring axis in the world frame** (1.9). Larger than item 3 and it subsumes it: it kills the
    whole class of camera-tracking artefacts rather than easing one symptom, and the review already named it as
    the real answer.

Deliberately excluded from this list despite being small: flipping `SSWorldFieldSurfaceTop` /
`SSWorldFieldCoverage` on (4.6) and re-enabling `SSAtmoWindFlowWorkerGL` (6.2) — each costs a build to judge and
one of them has a crash history that is not ours to fix.
