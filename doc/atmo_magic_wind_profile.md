# Atmo Magic: wind profile (altitude-varying wind) and the True Ground removal

Status: **design** (synthesized 2026-09-05 from two competing designs + adversarial review), except §2 (True Ground removal) which is implemented. Companion docs: `atmo_magic_storm_dynamics.md` (consumes `windAt(z)` — shear is the supercell ingredient), `atmo_magic_far_clouds.md`, `atmo_magic_debug_views.md` (V1 Wind Profile view ships with phase 2 here).

## 1. Why

Today the atmosphere has exactly one altitude behaviour: `SSWindFlowMap::windGradientScale(z_agl)` — a magnitude-only boundary-layer power law `clamp((z/10m)^alpha, 0.35, 3.0)`, plateau above 1500m. Direction NEVER varies with height (authored wind is horizontal, elevation hard-zeroed at `ssatmoenvbridge.cpp:110`). Three consequences:

1. **The deck drifts at the wrong altitude.** `cirrusAltitudeMetres()` is the only altitude ever passed to `windGradientScale` (`ssatmoenvapplier.cpp:524`), and the resulting `mCloudDriftM` accumulator is read by the dome AND the entire volumetric deck (base often 1–3km) AND `precipNoiseAt`'s air-frame conversion. The deck is advected at cirrus speed.
2. **No shear = no supercells.** Directional shear with height (a hodograph) is what organizes a storm into a persistent rotating updraft; the anvil must spread downwind at *anvil-level* wind, which can differ in heading from the surface wind.
3. **The old ground reference was heavyweight.** The power law measured `z_agl` from the worldfield's real-geometry True Ground capture — a per-texel GPU depth-peel product with tall-structure voiding and ring gap-fill, async, region-keyed, ~200 LOC — for a job ("roughly where is the ground") that a per-track constant answers.

## 2. Ground reference: True Ground removed  ✅ implemented

Two consumers used the True Ground grid, and they get two different cheap replacements:

- **Atmosphere-scale AGL** (the profile's `z_agl`, formerly `SSWindFlowMap::groundRefZ()`): `SSAtmoMagic::groundZero()` — the active track's floor Z (`cfg.mHasGround ? cfg.mGround : trackFloor`). Per-track authored constant, identical on every client, O(1), and already the reference that water height, cloud-field base heights, and dome altitude are expressed against — one vertical reference for a whole sky build instead of two. No async "tile not ready" fallback branching.
- **Flowmap slab AGL reference** (`Tile::mGroundRef`, the per-slab ambient power-law baseline): a low-res terrain average — `resolveHeightRegion` sampled on a coarse grid, each sample floored to water height, averaged once per full tile build. The flowmap's own geometry probes already model buildings; `mGroundRef` was only ever a scalar baseline, so the per-texel capture, structure-voiding and gap-fill bought nothing here.

Deleted: `SSWorldField::buildTrueGround` + the worldfield debug view's True Ground mode, `SSWindFlowMap::refreshTrueGroundClaim`/`mTrueGroundClaim`/`mTrueGroundRegion`, `Tile::mGroundZ` and its resample block, `groundRefZ()`'s grid lookup. `buildSurfaceGrid`/`SURFACE_TOP` stay (shared with the wet surface field). `windAlpha()` survives — it derives from the probe captures (`mTop`), not from True Ground (verified), though §3 removes it from the *atmosphere* path anyway.

## 3. The profile model (derived-first, with authored override)

A small closed-form hodograph, owned by the env side (per-track weather-cube property, like moisture/convection), NOT by the flowmap:

```
windAt(z) -> (heading, speed) as a 2D vector
```

**Inputs — chosen for determinism, this is where both original designs failed:**
- The **curve-resolved** wind at phase (`SSAtmoEnvWeatherState::mWindHeading/mWindSpeed`), NOT the eased `SSAtmoMagic::mWind`. The eased value is Euler-integrated with per-frame `dt` (`ssatmomagic.cpp:270`) — framerate changes its trajectory, so it must never feed anything that positions world content. The eased value stays for feel-only consumers (audio, particles, flexi).
- An authored/derived **shear exponent** on the weather cube, NOT the flowmap's `windAlpha()` — alpha is derived from the *camera's region's* geometry probes, so two clients in different regions would disagree about the sky. `windAlpha()`/`windGradientScale` survive only inside the flowmap's own slab math.
- `agl = z - groundZero()` (§2).

**Speed:** keep the tuned power-law shape below 1500m (same constants, new exponent source). Above 1500m a jet continuation gated by `shearStrength` S∈[0,1]: `scale(z) = 3.0 * (1 + S * 1.5 * smoothstep(1500, anvilAgl, z))`, clamp [0.35, 6.0]. S=0 ⇒ flat 3× exactly like today; note the auto derivation floors S at 0.15, so real skies always carry a small jet gain and veer (decided: physics by default, the gust-auto precedent).

**Direction:** total veer split between the friction layer and the free troposphere:
`heading(z) = heading10 + veerDeg * (B * smoothstep(0, 1500, z) + (1-B) * smoothstep(1500, anvilAgl, z))`, B ≈ 0.35 (tunable, art pass expected). When *blending between two sampled band vectors* (e.g. per-drop precip tilt), lerp the **vectors**, never the headings — bearing lerp is undefined at the wrap.

**Auto-derivation** (default, mirrors the `mGustAuto` idiom exactly — no new UI pattern):

```
storm         = smoothstep(0.55,0.85,moisture) * smoothstep(0.45,0.75,convection)   // the existing consolidation figure
shearStrength = clamp(lerp(0.15, 1.0, max(convection*0.4, storm)), 0, 1)
veerDeg       = lerp(8, 65, shearStrength) + clamp((15 - temperatureC)*0.4, -10, 15)
```

A dry calm sky veers ~9°; a consolidated storm day approaches 65–80° — real supercell range. When auto is off, `mShearStrength`/`mVeerDeg` are ordinary keyframed curves. (The 0.25–0.60 wet band / consolidation window is now duplicated in one more file — same accepted convention as the existing three.)

Rejected: the authored-first 4-level hodograph table (explicit per-level heading/speed keyframes). More authoring surface, and its closed-form-drift companion (baked cumulative tables) teleports the air frame on track switches and curve edits — the whole deck repops. Two scalars + auto derivation express the same weather stories.

## 4. Drift: one accumulator + bounded shear offsets

The critical review finding: **drift must stay an accumulator; shear must stay bounded.**

- **One integrator**, at the **deck-base** altitude (this alone fixes bug §1.1 — evaluate the profile at `mPrimary.mBaseZ`, not cirrus). Cross-client accumulator divergence from **session-start seeding** remains accepted debt for the shapeless cloud pattern only — nothing positional may ever live in the drifted frame (hard rule inherited by `atmo_magic_storm_dynamics.md`). The **dt-sum** divergence is no longer debt: the accumulator integrates in F64 (phase 4c, 2026-09-05); `scenario_drift_f64` measures 30 vs 144 fps over an hour at 233.75m in F32 versus 0.0000005m in F64.
- **Every other altitude is a bounded closed-form offset**, never a second integrator: `O(z) = shearSpanM * ramp(z)`, where `shearSpanM` is a capped function of the hodograph (cap ≈ 2500m, ~10×`CELL_M`). Two free-running accumulators at different velocities diverge without limit and their independent 1e6-m wraps snap the difference — rejected.
- **Frame contract** (revised after the phase-4 review, 2026-09-05): the cell **gate**, presence and the column's tower/anvil classification read base drift only (one frame, unchanged, constraint-5 lockstep intact) — the ground-shadow bake and `precipNoiseAt` therefore never see `O(z)` at all. `O(z)` is a **placement** transform: a puff of column c at altitude z is drawn at c + `O(z)`, and every fragment-side read of that puff at altitude z — the `n_map` column read that drives the tower window and anvil, the detail octaves, the triplanar skew, the cap band — **undoes** it (`frameAir = air − S − O(z)`), so geometry and carve classify the same column. The veil sheet is base-anchored by definition and reads everything at `gateAir` (no `O`); the shadow bake and `precipNoiseAt` are base-anchored too and never evaluate `O(z)`. (The 4b draft that read `n_map` at `gateAir` while placement carried +`O(z)` was inverted and is withdrawn.) The GPU never evaluates the profile: the CPU bakes `O(z)` into an 8-entry altitude table per build (inputs from the pure `windProfileAt` path and the *authored* dome height, never `cirrusAltitudeMetres()` which reads a per-client setting and the live deck) and both sides read it with one explicit `a + (b − a)·t` lerp spelled identically (`lerpExact`), pinned bit-exact by `twin_deckframe` — the earlier ~2mm figure was measured against `std::lerp` vs GLSL `mix()` and no longer applies.
- **Dome seam:** the dome reads `baseDrift + O(cirrusZ)` — bounded offset, so deck lid and cirrus band can never slide apart unboundedly.
- **Wrap fix (pre-existing bug):** `fmodf(drift, 1e6)` is not a multiple of `CELL_M` (260) or the noise tile (2048×scale) — today's wrap already repops the field. ✅ Wrap on the lcm of every air.xy period (cell 260, shader `SS_NOISE_M` 880 / `SS_SKEW_M` 1400, both decks' cell-quantised noise tiles; 2048 → 2080) = 800800m, about the old cadence. **Honest caveat found in review:** alignment keeps the tiling *textures* put, but the hash-based cell gate and cluster noise are not periodic in the cell index, so they still re-roll at a wrap. Full invisibility needs the builder, `ss_cell_occupied` and the shadow bake to hash the cell index modulo `span/CELL_M` — scheduled with phase 6b, which already reworks that replication set.
- **Gust seed fix (pre-existing bug):** `mWindDriftSeeded` latches on the first frame with speed > 0 — gust phase is session-start dependent. Seed from wall clock unconditionally.

## 5. Consumer migration

| Consumer | Change |
|---|---|
| Deck cell gate / shadow bake / `precipNoiseAt` | unchanged mechanism, now base-altitude drift (bug fix) |
| Anvil-height puffs / carve | `+ O(z)` frame transform, replicated at its two sites (placement, fragment frameAir) |
| Dome cirrus drift | `baseDrift + O(z_band)` where `z_band` = the band's own **current** altitude (`cirrusAltitudeMetres()`). Decided 2026-09-05 (Q: dome/deck unison): no rigid pinning needed — the anvil ramp already drags the band's altitude onto `cloudTopZ()+gap` during storms, so band and anvil sample the *same* profile point exactly when they are the same layer, and diverge only on fair days when the cirrus genuinely rides kilometres above the deck (physically right) |
| Precip tilt (~9 `windAt` sites) | flowmap sample stays first near geometry; flat fallback becomes `windAt(drop.z)`, per-drop vector-lerp between two precomputed band vectors |
| Particles, flexi, audio, snow, gusts | stay on eased near-surface wind, unchanged |

## 6. Control surface

- Weather Influence row **"Wind Shear"** (enable + strength) in `floater_ss_atmo_weather_influence.xml`; strength scales `veerDeg`/`shearStrength` effect; 0 ⇒ exactly today's uniform wind.
- `mShearAuto` checkbox + two keyframe rows (`shear strength`, `veer`) beside Convection/Moisture in the weather conditions panel, gust-row idiom.

## 7. Phasing

1. True Ground removal (✅ done, standalone).
2. ✅ `windAt(z)` + auto derivation (`SSAtmoEnvApplier::windAt`/`windProfile`, `SSAtmoEnvWeather::mShearAuto/mShearStrength/mVeerDeg`, resolved on `SSAtmoEnvWeatherState`), wired into the drift as a behavior-preserving swap on calm days (exponent = `EXPONENT_DEFAULT`, the flowmap's `windAlpha` is slab-only now).
3. ✅ Base-altitude drift fix (`mDriftBaseZ` = live primary deck base, else the authored base) + lattice-aligned wrap (`wrapSpanM(CELL_M, noiseTileMetres())`, the deck's noise tile quantised to whole cells in `SSVolCloud`) + unconditional wall-clock gust seed. The dome seam (`cirrusDriftMetres()` = base drift + `shearOffset(z_band, z_base)` × the Wind Scroll influence share) landed here too, since it is CPU-only; the deck-side `O(z)` replication is still phase 4.
4. `O(z)` shear transform, two-site replication (placement, fragment frameAir); precip tilt migration.
5. UI rows + art pass on B / veer constants.

## 7b. Tuning observation (visual test, 2026-09-05)

`visual_deck_frames` (`out/frames_anvil_lean.png`) shows that with `SHEAR_LEAN_S = 600` a storm-day profile reaches the 2.5km `SHEAR_CAP_M` by the boundary-layer top: everything above ~1500m becomes a rigid 2.5km offset instead of a graded lean, so the tower reads as "kinked then vertical". Expect the art pass to lower `SHEAR_LEAN_S` to ~150–250s (anvil lean ~1–1.6km, below the cap) or make the cap the anvil-level value with a smooth approach. Both are constants in `sswindprofilecore.h`; the twin/unit tests pin only invariants, not these values.

## 8. Open questions

1. ~~Dome/deck heading unison~~ **Answered 2026-09-05**: anvil top and cirrus are the same layer when a storm stands — and the existing altitude convergence (anvil ramp pulling the band onto the deck lid) makes unison emergent: the dome samples `windAt` at its own current altitude, no coupling rule needed (§5 dome row).
2. Shear cap: is a 2.5km anvil lean enough to read as a supercell at 10km draw, or should shear exceed the cell grid (forces gate-level rework)?
3. Fixing the existing wrap/seed determinism bugs changes current visuals slightly on every client — land with the profile, or separately?
