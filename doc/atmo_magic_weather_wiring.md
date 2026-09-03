# Atmo Magic: weather-field wiring — the audit and the reconnections

2026-09-03. A matrix audit of what raw moisture / convection / temperature actually drive found
several effects wired to raw convection or a flat authored value where the rest of the system had
moved to gated, derived drives — the same class of disconnect the retired scene-gamma/ambient
darkening had (convection dimming the world with no wet sky required). This records what was
reconnected and why, and what was inspected and deliberately left.

## The rule the fixes follow

Convection alone is clear-air turbulence. Any effect that implies condensed water — storm
darkening, thunder, cloud gloom — must gate on moisture as well. The canonical wet band is
**0.25–0.60 moisture** (the sky modulator's `DARKENING_MOIST_MIN/FULL`); the lightning resolver
(`LIGHTNING_MOIST_MIN/FULL`) and the cloud deck's gloom (`GLOOM_MOIST_MIN/FULL`) now carry the
same numbers, kept in step by hand, so the sky that flattens into a storm is the sky that
thunders and the sky whose puffs char.

## Reconnected

**Lightning wet gate** (`ssatmoenvweatherstate.cpp`): auto-mode intervals are now scaled by
`lightningMoistureScale()` — zero below the band's onset (a dry okta-0 sky at SEVERE convection
used to strike every 2–5 s out of clear blue), ramping in like the winter season scale, which it
multiplies. The no-environment fallback in `sslightning.cpp` mirrors it through
`atmo->precipitation()` (the cube's moisture verbatim).

**Manual lightning intensity fires** (`ssatmoenvweatherstate.cpp`): with `mLightningAuto` off,
the authored intensity used to set only fierceness while intervals still came off the convection
phase — a keyframed storm under a calm sky never fired one bolt. Intensity now maps straight to
cadence (a whisper ≈ once a minute, full = SEVERE's 2–5 s), season still stretches it, and there
is no wet gate: an override is an order, the same call `mPrecipitationOverride` makes.

**Bolt-from-the-blue season** (`sslightning.cpp`): the anticipation clock was the one interval
path that skipped `lightningTemperatureScale` — winter approach bolts fired at summer cadence
while ordinary strikes stretched up to 20×. It divides by the season now.

**Cloud deck gloom** (`ssatmoenvcloudfieldstate.cpp`): `mGloom` was raw convection, ungated —
the second "storm darkening" sharing the UI label with the sky modulator's toggled, moisture-gated
one. It now rides the same influence row (`mStormDarkeningEnabled/Strength`, off = authored
albedo) and the same wet band, so a dry heatwave's thermals no longer char fair-weather puffs and
the Weather Influence toggle finally governs both halves of the effect.

**Band-graded drop size and splash strength** (`ssprecipitation.cpp`, plumbed
bridge → `SSAtmoTrackConfig::mDropletScale/mImpactScale` → `SSAtmoMagic::dropletScale()/impactScale()`):
the weather cube's per-band tables (`mDropletSizeScale`, `mImpactScale` — drizzle 0.05/0.00 up to
torrential 1.0/1.0) were computed and never read anywhere; drop sizing reimplemented its own ramp
off raw precipitation and splash strength was a flat per-preset constant. Presets now opt in per
effect (`mWeatherSize`, `mWeatherImpact`, both default on, checkboxes in the preset editor's
Density and Impact tabs). Values stay in range by construction: the band drives the *same*
0.55–1.45 size-multiplier window the old ramp used (and still needs `mIntensitySize` > 0), and
impacts scale the authored strength 0..1 — drizzle stops queueing impacts entirely rather than
queueing zero-strength ones. The runoff-clump landing keeps the flat authored strength: a
gathered stream hits as hard as its volume says, not as hard as the sky currently falls.

**Live-warmth snow melt** (`sssurfacefield.cpp`): `tick()` never saw temperature — whether snow
grew or melted was decided once by which preset resolved, and melt ran at a fixed rate however
warm it got. The melt rate now scales with `temperatureC()`: the authored rate means "a mild
+10 °C day" (`MELT_FULL_C`), real warmth runs up to double (`MELT_WARM_MAX`), and sub-zero air
holds the pack with a whisper of sublimation (`MELT_SUBLIMATION` = 0.02) so an abandoned drift
still leaves eventually. Wetting, drying and drainage stay temperature-blind — a cold puddle is
still a puddle.

## Inspected and deliberately left

- **The dormant "Cloud Cover" influence row** (`mCoverTarget`/`mCoverBlend` pinned to 0): the
  sliders and serialisation are sound; the dome↔deck coverage coupling it gated is parked until
  the horizon/colour issues that drove it off are reworked. The row stays as the reattachment
  point.
- **The differing cold thresholds** (regime 1.5 °C hardcoded, squall 0 °C, lift via
  `SSAtmoSnowLiftTemp`, corona −4 °C, frost 0→−20 °C): read as deliberate per-phenomenon tuning,
  not drift. Not unified.
- **Precipitation intensity = raw moisture** and the deck's own convection ramps (anvil windows,
  storm consolidation): plausible per-effect shaping; no shared cutpoint table imposed. Moisture
  being the sole answer to *whether* it rains was the one part of this that did not survive: see
  the `Falling` switch in doc/atmo_magic_env_ui.md, which suppresses precipitation without touching
  any of the cloud, gloom or lightning drives recorded above.

## Follow-up: the gloom's drive and where it is spent (same branch)

The audit above put the deck's gloom behind the wet band and the influence toggle, which was
right, but left two things that between them made Storm Darkening read as doing nothing at all.

**Convection was still a gate, and it counted twice.** `mGloom` multiplied by raw convection, and
the auto darkening figure it multiplied (`deriveAutoBaseline`) was itself convection-led
(`0.45 + 1.25c + 0.3mc`). A deck therefore had to be both soaked *and* violently rising before it
lost a shade — which excludes the one sky that most needs it, since a nimbostratus is a moisture
regime and not a convective one. The heaviest stratiform rain deck the system can build resolved
to gloom 1.00, the same albedo as fair-weather cumulus. The drive is moisture-led now
(`0.45 + 1.05m + 0.5c`), convection survives as a modifier (`turmoil`, 0.6→1.0) rather than a
gate, and the deck's own **thickness** joins it (`depth`, 0.35→1.0 over the storm lid): the same
water through 200 m and through a kilometre of cloud are not the same cloud, and "darker due to
its contents" is water × depth. The extremes are unmoved — soaked and severe still lands on
darkening 2.0, where the old curve topped out.

**It was spent flat.** `ss_gloom` multiplied every fragment of the deck equally
(`ssVolCloudF.glsl`), so it moved the exposure and left the shape alone, which the eye reads as no
change. It is graded over a new per-puff **buried depth** now — `SSVolCloud::Puff::mBuried`, the
fraction of a puff's own column standing above it, carried on the spare green vertex channel — so
the lid keeps its light and the belly loses it, and darkening a deck *deepens* it. At gloom 1 the
grade is the identity, so fair weather is untouched.

The second half also fixes something that was not a gloom bug: the builder's per-puff column
shading (`Puff::mForm`, the exponential shade through the cloud above each puff) was being washed
out by the unshaded ambient, which is the larger term under any overcast. A lit top came out a
tenth brighter than its own base instead of several times. Grading the ambient is what lets that
existing algorithm show.
