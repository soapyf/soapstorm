# EEP atmosphere — the algebra, and reference values

What the windlight/EEP sky parameters actually compute, deconstructed from `skyV.glsl` / `atmosphericsFuncs.glsl` / `lllegacyatmospherics.cpp`, and the authoring rules that fall out of the equations. Stock math is pristine in this fork (SS_ATMO touches none of it); this document exists so presets are authored against the real equations instead of folklore. All authoring values below are FRONTEND units (what the sky editor shows). May drift; the shaders win.

## Frontend vs backend units (llpaneleditsky.cpp)

The editor rescales several values before storage; the equations below use BACKEND values, the reference tables use FRONTEND. Getting these mixed up is catastrophic in both directions - a backend density typed into the UI is a 1000x error that renders the sky as black void (no atmosphere at all).

| Parameter | Conversion | Stock default (UI) |
|---|---|---|
| Density multiplier | backend = UI × 0.001 | 0.18 |
| Blue horizon, blue density (swatches) | backend = UI × 2 | bd ~(0.12, 0.13, 0.22), bh ~(0.25, 0.25, 0.32) |
| Ambient, sun/moon colour (swatches) | backend = UI × 3 | — |
| Glow size | backend glow.x = (2 − UI) × 20 | 1.75 |
| Glow focus | backend glow.z = UI × −5 | ~0.1 |
| Haze density, haze horizon, distance multiplier, max altitude | direct, no scaling | 0.7 / 0.19 / 0.8 / 1605 |

Glow is the trap of the set. `haze_glow = ((1−dot(dir,sun))·glow.x)^(−5·focus)`: UI size LARGER → backend glow.x SMALLER → brighter, wider halo — until glow.x nears 0 and the negative power diverges: sizes ~1.85+ WHITE OUT the entire frame (verified). The stock 1.75 sits just under the cliff; modest sunset halos live around 0.4–0.8 (verified: size 0.5 + focus 0.06), with focus 0.05–0.15 for soft bloom and higher for a hot tight core.

## The pipeline, in five equations (backend units)

With `bd` = blue_density (RGB), `hd` = haze_density (scalar), `bh` = blue_horizon (RGB), `hh` = haze_horizon (scalar), `dm` = density_multiplier, `distm` = distance_multiplier, `Y` = max_y, `amb` = ambient (× cloud-shadow lift):

1. **Weights** — `bw = bd/(bd+hd)`, `hw = hd/(bd+hd)`. The two haze species split every ray's scattering budget; raising one *takes share from the other*.
2. **Sunlight extinction (per ray)** — `sun' = sun × exp(−L / (ray_y + sun_y))` where `L = (bd + hd/4) × dm × Y`. The denominator is ray elevation plus sun elevation, so a low sun kills sunlight across a band of low sky that CLIMBS as it sets. `L` is chromatic through `bd` (blue dies first → red sunsets) and achromatic through `hd` (greys everything equally).
3. **Path saturation** — `atten = exp(−(bd+hd) × len × dm × distm)`; for the sky dome `len = Y / sin(elevation)` (every ray extended to the slab top), for in-world geometry `len` = slant distance. Where `atten → 0` the ray shows pure airlight; where `atten → 1` (near-zero density) there is NO airlight and the sky is black space.
4. **Airlight (additive)** — `A = [ bh·bw·(sun' + amb) + hh·hw·(sun'·glow + amb) ] × (1 − atten)`, with `glow = ((1−dot(dir,sundir))·glow.x)^glow.z + 0.25`. Final sky colour ≈ `A` (the dome is airlight); fogged objects → `colour·atten + A×2`.
5. **Scene "gamma"** — `out = 1 − (1 − 2·colour)^γ`. A soft-clip exponent, not display gamma: γ=1 is a linear doubling, big γ lifts toward white, small γ crushes darks. 0.06 is a legitimate moody choice.

## The invariants (what the algebra guarantees)

- **The horizon band IS the ambient.** Where the sun has extinguished and the path saturated, eq. 4 collapses to `A = (bh·bw + hh·hw) × amb`. You cannot remove that band; you can only *paint* it. Every celebrated SL sunset paints it warm: **ambient (and blue horizon) go gold/salmon at the dusk keyframes**, and the "glow reaching the sea" is authored ambient wearing the haze weights.
- **Redness is the bd:hd ratio.** Chromatic bd reddens the low sun; achromatic hd greys it. A muddy sunset means hd is stealing the extinction budget.
- **Density multiplier is double-coupled** (both eq. 2 and eq. 3): raising it extinguishes the sun sky-wide *and* fogs everything; dropping it toward zero deletes the atmosphere (black-void sky). Distance multiplier lives only in eq. 3 — it is the *safe* "how far can I see" dial.
- **Max altitude trades band height for everything.** It scales `L` (band height) and every sky ray's `len` (haze altitude). Lower compresses the haze to the horizon but weakens sun extinction — use it for band shaping only with the rest re-balanced.
- **The band's height formula**: sunlight survives above `ray_y ≈ L/τ − sun_y` (τ ≈ 3 for "visibly alive"). Predicts exactly how the blue/gold band climbs as the sun sets.

## Reference values (FRONTEND units)

Baseline (clear midday): blue density (0.12, 0.13, 0.22) · blue horizon (0.25, 0.25, 0.32) · haze density 0.7 · haze horizon 0.19 · density multiplier 0.18 · distance multiplier 0.8 · max altitude 1605 · glow size 1.75, focus 0.1 · gamma 1.0 · ambient neutral ~(0.12, 0.12, 0.13).

Sunset keyframe (the band painted gold): **ambient (0.30, 0.18, 0.12)** · **blue horizon (0.27, 0.21, 0.19)** · haze horizon 0.35–0.5 · blue density (0.10, 0.12, 0.22) (keep blue-dominant!) · haze density 0.4–0.6 · density multiplier 0.15–0.25 · distance multiplier 1–2 · max altitude ~1000 · glow size 0.5, focus 0.06 (verified broad soft sunset halo) · gamma to taste. The day cycle animating ambient/blue horizon warm at dusk and back is the entire trick.

Sunset, blood-red variant (the whole sky a crimson wall, dark sea, small hot disc): same structure, opposite palette philosophy - SATURATED and DIM where the golden variant is warm and bright. **ambient (0.10, 0.03, 0.02)** · **blue horizon (0.30, 0.08, 0.05)** (red-dominant - the "blue" name is only its default) · haze horizon 0.5–0.8 (band thickness) · **haze density 0.25–0.4** - the critical one: the achromatic haze pastelises red toward cream, so chromatic purity demands it LOW, and the band's body comes from haze horizon instead · blue density (0.10, 0.12, 0.22) · density multiplier ~0.2 · glow size ~0.3, focus 0.2–0.35 (small hot disc, tight halo) · cloud colour near-black (silhouettes) · gamma 0.5–0.8 (dark floor).

Overcast/storm: haze density 1.5–2.5 · distance multiplier 2–4 · ambient down and grey · cloud colour dark · density multiplier stays 0.15–0.3 — express murk through haze/distance, never density.

Night: ambient low blue-grey; everything else matters little once `sun'≈0` — the sky is `(bh·bw+hh·hw)×amb` wall to wall.

Red flags: distance multiplier < 0.3 (starves every distance cue — deck, dome and fog can never meet) · density multiplier ≪ 0.1 (atmosphere vanishes, black-void sky) or > 0.5 (sun extinct everywhere) · haze density > 1 on a preset that wants red sunsets.
