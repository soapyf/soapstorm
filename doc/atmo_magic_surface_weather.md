# Atmo Magic: surface weather materials and screen weather effects

Design for the additive surface material layer the weather applies to the world - wetness, rain
drops and drips, puddles and their impact rings, ice, frost, deposits (snow, sand, dust, ash) and
coloured liquids (ink, blood, mercury) with the stains they leave - and the three screen-space
effects that go with it: the height fog layer, the heat shimmer and the lens drops. Written
2026-09-06 on `feature-surface-weather`; the cores are `sssurfacestatecore.h` and
`ssscreenfxcore.h`, the harness is `V:\Scratch\surface`.

Everything here is additive to the surface field that already exists (`sssurfacefield.*`,
`doc/atmo_magic_snow.md`, `doc/atmo_magic_snow_architecture.md`): the per-region cell ledger of
wet / puddle depth / deposit depth, the camera-centred field window the shaders read, the wet
pass (specular), the normal-flatten pass (with drainage ripples), the snow pass (albedo) and the
commit pass that writes them back into the gbuffer ahead of lighting. What is new is the STATE
those channels take on, the looks they wear, the fragment-level detail (drops, drips, rings,
cracks, grain) the metre-grid cannot carry, and the screen layers.

## 1. What exists, what changes

| Piece | Today | After |
|---|---|---|
| Field channels | `mWet` 0..1, `mPuddle` m, `mSnow` m (+ lift, store, scorch) | + `mIce`, `mFrost`, `mStain`, `mAge` (all 0..1) stepped by `SSSurfaceState::stepCell` |
| Field windows | `ssFieldMap` (Z, wet, snow, puddle), `ssFieldFlowMap` (slopeX, slopeY, slopeNorm*wet, spare) | + `ssFieldStateMap` (ice, frost, stain, age) - same lattice, same origin uniform |
| Looks | preset tint applies to particles only | `LiquidLook` and `DepositLook` per preset, crossfaded as the new kind accumulates (`Mix`) |
| Wet pass | roughness/gloss tightening, puddle mirror | + porosity-aware tightening, ice, frost, deposit matte, liquid metal |
| Snow pass | albedo lift toward one hardcoded white, hash glint | becomes the **albedo pass**: wet darkening (porosity exponent), liquid tint, stain, ice, frost, deposit palette, thickness, age |
| Normal pass | flatten toward up, drainage ripple scroll | + static drops, drips, sheet flow, ice freeze, deposit rounding, impact rings |
| Whiteout | one fog veil in the pool pass, two demands (squall, lift) | retired; rewritten as the **post-process height fog layer** (`ssheightfog.*`, `ssPostFogF.glsl`): + ground fog, precipitation veil, coloured mist, wisps, sun scattering |
| Post chain | nothing of ours | heat shimmer (`ssPostHeatF.glsl`) and lens drops (`ssPostLensF.glsl`), owned by `SSScreenFXPost` (the class - `SSScreenFX` in `ssscreenfxcore.h` is the shared core namespace, twinned by the harness, not this class) |

Rules carried over unchanged: the field is the one ground truth; ground state steps in fixed
quanta of shared time; anything positional lives in the world frame; alpha in the post-deferred
screen target is the bloom mask (`doc/viewer/glow_and_alpha.md`) - the surface passes write the
G-buffer, where alpha is what each attachment says it is, and the post passes run after
`combineGlow`, where alpha is junk and stays junk.

## 2. Data model

### Cell state (`SSSurfaceState::CellState`, stepped in `tick()` after the settle pass)

- **Ice** - the frozen fraction of whatever water the cell holds. A full puddle at ice 1 is a
  frozen puddle; a wet film at ice 1 is glaze ("black ice"). Forms below 0 C at a rate that is full
  at -8 C (four minutes to freeze solid), thaws above 0 C (three minutes at +6 C), and fades out in
  half a minute once the water it froze has drained or dried.
- **Frost** - hoar frost, independent of precipitation: forms on exposed (sky-facing) cells when
  the air is below -1 C and humid, over fifteen minutes at full rate (below -10 C in saturated air);
  about an hour on a typical -4 C night; sublimates in warmth or sun over five; buried by any deposit
  deeper than 5 mm. This is the "make things look cold" channel - it does not need rain, only a cold
  humid night.
- **Stain** - the tint a staining liquid leaves. Sets while an ink-like liquid is wet on the cell,
  washes out under clean rain, and otherwise fades over an hour (the stain follows the liquid that is
  falling, not the ground's crossfaded look).
- **Age** - deposit compaction, 0 fresh to 1 old. Fresh fall un-ages a pile in proportion to how
  much of it is new; warmth ages it faster toward slush. Drives the deposit's tint toward grey and
  its sparkle down.

The tick order per cell: settle (existing: wet, snow, puddle) -> `stepCell` -> `washRemoval`
(liquid precipitation removes a washable deposit: ash and dust wash away, snow melts instead) ->
granular transport (existing). Inputs the shell assembles once per tick: temperature
(`SSAtmoMagic::temperatureC`), humidity (the weather cube's moisture where it is reachable, else
`max(precipitation * 2, peak wet)` clamped - stated in the shell), sun-up factor (sun direction Z
clamped, times the cell's exposure), the resolved looks.

### Looks (`LiquidLook`, `DepositLook`, `Mix`)

The field holds ONE liquid and ONE deposit at a time. Each preset carries both looks (new LLSD
keys `liquid_tint`, `liquid_opacity`, `liquid_stain`, `liquid_metal`, `deposit_tint`,
`deposit_sparkle`, `deposit_translucency`, `deposit_depth_full`, `deposit_wash`,
`deposit_melts`). When a preset with a different look starts falling, the field's `Mix` crosses
from the old look to the new one as the new kind ACCUMULATES (`advanceMix`: t rises by the tick's
gain over the greater of what is present and the channel's "full"), never on the preset switch
itself - a switch with nothing yet fallen must change nothing on the ground. At t 1 the new look
is promoted and the mix resets. The resolved looks go to the shaders as uniforms.

Built-in presets: Rain (water), Snow, Blizzard, Diamond Dust, Hail (water liquid, snow deposit),
Sand (tan, low sparkle, opaque, never melts, washes slowly), plus new Ink Rain (LIQUID, near-black,
opacity 0.95, stain 0.8), Ash (FLAKE, warm grey, sparkle 0, opaque, never melts, washes 1) and Dust
(FLAKE, ochre, sparkle 0.1, washes 1). Blood rain and mercury are one preset each away (a tint and
opacity; metal 1 for mercury) and are not built in.

## 3. The three passes and their shared evaluation (as built)

`ssSurfaceStateF.glsl` is a new include every surface pass links, beside `ssSurfaceFieldF.glsl`
(link order in every one of the three `mShaderFiles` lists: field, then state, then the pass
file). It does NOT hand the pass files a struct - a `struct` field only exists inside the
translation unit that declares it, and each pass file is its own compiled shader object
(`LLShaderMgr::loadShaderFile` runs one `glCompileShader` per `mShaderFiles` entry, all linked
into one program afterward). GLSL link resolves a function prototype against a body defined in
another unit, and merges matching `uniform` declarations across units - it does not propagate a
`struct` type, a `const`, or a fully-defined function body. So the include instead exposes a set
of free functions and per-fragment law functions (`ssFieldFetchState`, `ssPorosity`/
`ssPorosityLegacy`/`ssPorosityAt`/`ssPorosityFromAlbedo`, `ssWetAlbedoExponent`, `ssDropLaw`/
`ssSheetLaw`, `ssDepositCoverage`/`ssDepositSoften`/`ssDepositSparkle`/`ssDepositMask`,
`ssRingHeight`/`ssRingSlope`, `ssWorldNoise`, `ssSparkleCell`, `ssFieldWindCarry`,
`ssPuddleMaskNoise`/`ssPuddleLatHash`, `ssSurfaceLevel`) plus the `SS_*` LOCKSTEP constants
(twinned bit-for-bit with `sssurfacestatecore.h` by `twin_surface_glsl.cpp`) and the uniforms the
shell uploads (the resolved looks, the toggles, `ssTime`, `ssSurfaceWind`).

Every one of those - functions (as prototypes), constants and uniforms alike - has to be
re-declared in each pass file that actually calls or reads it, because that pass file is a
separate compilation unit from the include. `diffuseRect`/`specularRect` follow the same rule:
declared once in the include, and again in whichever pass file samples them directly (wet and
albedo read both; normal reads `diffuseRect` only, for `ssPorosityFromAlbedo`). Uniform *values*
are a different story from uniform *declarations*: a linked program resolves uniform locations
against the whole program, not the file that happened to declare them, so the shell has to upload
every uniform any linked unit reads for a given program - including ones a pass file's own text
never mentions, such as `ssPuddleMaskScaleM` (read only inside `ssPuddleMaskNoise`, which lives in
the include, but still needs its own upload for `gSSSurfaceAlbedoProgram` once that program's
albedo pass file starts calling it).

Each pass then does only its own attachment:

| Pass | Writes | Reads | New responsibilities |
|---|---|---|---|
| `ssSurfaceWetF` | specular / ORM | spec, diffuse (porosity) | porosity-aware roughness (porous surfaces tighten less until wet 1), ice (roughness `ICE_ROUGHNESS`, dielectric spec), frost and deposit (matte, `FROST_ROUGHNESS` / `DEPOSIT_ROUGHNESS`, by coverage), liquid metal (ORM.b toward look.metal, legacy spec colour toward the tint) |
| `ssSurfaceAlbedoF` (was `ssSurfaceSnowF`) | albedo | albedo, spec (for porosity) | wet darkening `pow(albedo, wetAlbedoExponent)`, liquid tint by opacity over wet and puddle, stain (tint at `look.stain` strength), puddle floor (slight darkening), ice (pale blue-white, crack lines from a world-hashed cell edge pattern, glint), frost (white lift on up-facing, fine world-hashed grain, glint, gated by exposure so it does not grow indoors), deposit (palette tint, coverage scaled by the legacy `SSAtmoSnowSurfaceStrength` dial, translucency lets the albedo through at a dusting, age greys it, sparkle scaled by `depositSparkle`) |
| `ssSurfaceNormalF` | normal | normal, diffuse (roughness-free porosity) | existing flatten and drainage ripples; static drops on level sealed surfaces; drips on verticals; sheet flow at torrential; ice freezes the flow (`* (1 - ice)`) and adds the crack relief; deposit rounds the normal toward up by `depositSoften`; impact rings near the camera |

Order is **albedo -> wet -> normal** (`pipeline.cpp` draws `renderAlbedoPass()` first, then
`renderWetPass()`, which itself draws the wet program and then the normal-flatten program in one
function). This is NOT "so the later passes see a pristine input" - only the albedo pass sees
both the diffuse and specular attachments pristine, and that is the whole point of running it
first. The wet pass's own specular read is pristine regardless of order (nothing else in this
trio writes specular before it does). The normal pass runs last, after both the albedo pass has
tinted the diffuse attachment and the wet pass has darkened/tightened the specular one - so its
porosity estimate is deliberately the roughness-free `ssPorosityFromAlbedo` (diffuse variance and
luminance only), not `ssPorosityAt` (which additionally wants pristine specular). All three
passes therefore compute porosity from different degrees of "already weathered" input by
construction, not by oversight - stated here so a future change does not go looking for the one
true porosity value shared by all three.

The albedo pass runs whenever any of wet, deposit, ice, frost or stain has a non-zero peak (the
field tracks peaks for the new channels too) - that gate is independent of the legacy "snow
surfaces" setting, which now only scales the deposit layer's own visual coverage inside the pass
rather than returning out of the whole pass.

## 4. Wet material model

Water on a surface darkens it, saturates it and tightens its highlight - by how much depends on
what the surface is. A gbuffer has no material identity, so the passes estimate **porosity**
(`SSSurfaceState::porosity`) from what the gbuffer does carry: PBR roughness (rough = porous),
metallic (metal = sealed), albedo luminance (bright surfaces read a little less porous) and a
local albedo VARIANCE measure - four albedo taps one texel apart, the mean absolute difference
against the centre, normalised - which is the "how noisy does the surface look" analysis: gravel,
soil, weathered wood and fabric are noisy and porous; glass, gloss paint and polished stone are
flat and sealed. Legacy (Blinn-Phong) surfaces use the specular colour's brightness in place of
roughness/metal. The estimate is coarse and stated so; its job is to stop glass darkening like
soil and soil shining like glass.

- Albedo: `pow(albedo, 1 + WET_DARKEN_POW * wet * porosity)` - the Lagarde approximation of a wet
  porous material (darker AND more saturated), nothing at porosity 0.
- Roughness: the existing multiplier, applied at full strength on sealed surfaces and easing in
  with `wet * (0.5 + 0.5 * (1 - porosity))` on porous ones (a porous surface has to saturate
  before it shines).
- Metal liquids: `look.metal` pushes ORM.b toward 1 and the albedo toward the liquid tint at the
  puddle; a legacy surface gets the tint as its specular colour. A mercury puddle is a mirror
  with colour; a water puddle stays a dielectric.

## 5. Rain drops on surfaces

Detail the field cannot carry, drawn per fragment in the normal pass, world-anchored so it does
not swim with the camera, faded by the footprint (`fwidth`) the way the drainage ripple already is.

- **Static drops** on level, sealed surfaces (`level` high, porosity low): a world-XY grid at
  `~4 cm * dropLaw.size`, each cell hashes an offset, a radius and a birth phase; a drop is a
  spherical cap whose normal tilt is added to the shading normal. Density follows
  `dropLaw.static` - drizzle leaves a few, heavy rain covers the surface, and past
  `SHEET_INTENSITY_LO` the drops give way to sheet flow (they are no longer distinguishable in a
  film that is moving). Drops sit ON a wet surface, so they gate on wet as well as intensity, and
  a drop on a porous surface is absorbed (density times `1 - porosity`).
- **Drips** on verticals (`level` low): the Heartfelt technique in a triplanar frame -
  u along the horizontal tangent `cross(up, n)`, v along world Z. Cells `~6 cm` wide; each cell a
  drop sliding down at `dripLaw.speed` with the stutter function (the drop pauses and lurches),
  leaving a trail of small droplets that fade; density `dropLaw.drip`. The trail's normal is a
  narrow vertical ridge; the drop's a cap. Wind at the camera (a uniform) skews the trail
  direction a little toward the wind's tangent-plane component.
- **Sheet flow** at torrential intensity on any sloped wet surface: the existing drainage ripple
  term (the water-plane wave normal scrolling down-slope) applied by `dropLaw.sheet * wet`
  everywhere sloped, not only on drainage cells, so a roof in a downpour reads as a moving skin of
  water and its runoff lines join the existing shed cascades at the eave.

All three are animation (wall clock), never state; the laws that set their density and size are
in the core and twinned.

## 6. Puddles and impact rings

Puddle placement, shore carving and the mirror treatment stay as they are (the region-anchored
value-noise mask is the "global noise map anchored to the region"). Additions:

- The puddle FLOOR: a slight darkening and the liquid tint by opacity (an ink puddle is opaque and
  black; a water puddle shows the darkened floor).
- **Impact rings**: within `RING_NEAR_M` (4 m) of the camera, `SSAtmoMagic::processImpacts` records
  each impact into `SSSurfaceField`'s `RingBuffer` (the newest `RING_MAX`) instead of spawning a
  ripple quad, and the normal pass tilts the normal of puddle fragments by the analytic ring
  (`ringHeight` / `ringSlope`: a damped wave packet travelling out at `RING_SPEED_MPS`, crests
  `RING_WAVELENGTH_M` apart, gone after `RING_LIFE_S`) - the 4rknova raindrops-on-puddle look, but
  driven by the ACTUAL impacts the simulation landed, so the rings are where the drops fell.
  Outside the radius the ripple quads carry on. On non-puddle wet ground the ring is drawn at a
  quarter strength as a splash mark on the film. Only the RING is replaced: the splash crown still
  spawns (`SSPrecipSim::spawnCrown`), as does a hail impact's shatter burst - what changes near the
  camera is how the ring is drawn, not what the landing does.

- **One landing, one look.** The analytic ring and the landing ripple quad are the same event drawn
  two ways, and the two are held together at both ends:
  - *Rate.* `ringRate(quad_radius, quad_life)` reads the quad the preset and the taste controls
    (`SSAtmoRippleScale`, `SSAtmoRippleSpeed`) would have spawned and returns the rate the ring's
    own clock runs at, so both fronts travel the same metres per second. Every ring time in the
    core is on that clock; the shell multiplies the wall clock by the rate before the shader's
    `rt`, and divides `RING_LIFE_S` by it when expiring the buffer. The ring's larger extent
    (`RING_REACH_M`, 1.1 m against a default quad's 0.35 m - it has a whole puddle to cross) is paid
    for in TIME, not in speed: the same wave, running at the same speed, simply takes longer.
  - *Fade.* A crest thins as it spreads (`ringSpread`, `1/sqrt(r)` past `RING_SPREAD_R0_M`) and is
    eased to exactly zero over the last `1 - RING_FADE_FROM` of its life (`ringTaper`), so a ring
    leaves the buffer with nothing left to pop out of existence.
  - *Shading.* The quad is water, so `ssPrecipLitF.glsl` draws it as water, the same way
    `ssPrecipRainF.glsl` draws a falling drop: the ground beneath refracted through the wave's baked
    normal (scene map; a mid-grey guess without one), darkened by the wave's own relighting, mixed
    toward a glossy probe sample by water's fresnel, with the sun's glint added on top. Where the
    wave is flat the ground shows through untouched. The refraction offset is a world displacement
    over the fragment's depth, not the drop shader's flat screen-space offset - a ring is ground,
    not a billboard, so a fixed offset would smear the distant ones. It used to be a white ring
    painted over the top, which is what gave the two systems away as two systems.

  Two constraints hold that composition together, both from the same fact - the scene map is last
  frame's scene at this pixel and the quad drew into it, so alpha blending makes the frame-to-frame
  gain `1 - a + a * relight`: the relighting may only darken (`[0.4, 1]`), and the reflection is
  mixed in rather than added. Anything above unit gain held over a pixel compounds until the ring
  blows out white.

## 7. Ice and frost - the cold look

- **Frozen puddles**: as ice rises the puddle's mirror gives way to a frosted glass - roughness to
  `ICE_ROUGHNESS`, the flatten stays (ice is flat) but the drainage/sheet motion freezes
  (`* (1 - ice)`), rings are suppressed, the albedo lifts toward a pale blue-white with the floor
  still faintly visible (translucent), and a world-hashed cell-edge pattern draws crack lines and
  trapped-bubble grain. A glint like snow's, sparser.
- **Glaze**: a wet film at ice > 0 keeps its tight highlight (it is still smooth) but the albedo
  darkening eases off (ice is not absorbed) and a faint white edge grain appears - the surface
  looks slick and cold rather than merely damp.
- **Frost**: on up-facing surfaces, a white lift scaled by `frost * level`, with a fine
  world-hashed grain (needles) and a glint, and the specular goes matte. Vertical faces take a
  little at their tops through the exposure term. Frost forms without any precipitation on a cold
  humid night and burns off in the morning sun.

Temperature is one scalar per tick (the weather cube's); the settle pass's snow melt already
scales by it. Ice thaws and frost sublimates through the same figure.

## 8. Deposits - snow, sand, dust, ash

The old snow pass lifted the albedo toward one white and added a hash glint; the user's verdict
was "a slightly colder, whitened surface, not snow". Snow has to read as a LAYER lying on things.
One channel, one look at a time (section 2); three passes agree on where the layer lies through
one shared function:

- **The accumulation mask** (`ssDepositMask`, the Zucconi / MinionsArt construction): the field's
  coverage lowers a threshold the surface's up-facing (`n.z`) must clear, dithered by two octaves
  of world-anchored value noise. At low coverage only near-level surfaces take snow, in patches
  with feathered edges; as coverage rises the patches join and the threshold reaches down the
  slopes; vertical faces never clear it. Albedo, normal and specular passes call the same
  function with the same arguments, so the layer is one thing.
- **Albedo**: the look's tint (snow 0.93/0.95/0.99), greyed by age, with a 2 cm grain; at a
  dusting the surface's own albedo still shows through (translucency), a full layer hides it.
- **Normal**: the layer rounds the relief under it (`depositSoften`, up to `DEPOSIT_SOFTEN_MAX`
  at three times `depthFull`) and carries a grain micro-normal, footprint-faded so it never
  moires; drops and drips are gated off under it.
- **Specular**: the layer is matte (`DEPOSIT_ROUGHNESS`) and covers the baked occlusion under it
  (ORM red to 1); the **sparkle is real and view-dependent** - about 3.5 percent of 2.5 cm
  world-hashed cells get a mirror-tight lobe, so highlights pop in and out as the camera moves,
  which is what snow does and what an albedo glint never did. Sparkle strength comes from the
  look (snow 0.7, sand 0.15, ash 0) times age (`depositSparkle`).
- **Age**: fresh is white and glints; an aged pile greys and loses `SPARKLE_AGE_LOSS` of its
  sparkle; warm-aged snow is slush (the age rate scales up above freezing).
- **Removal**: snow melts (the settle pass, scaled by `look.melts`); ash and dust are washed off by
  liquid precipitation (`washRemoval`) and blown by the granular transport as before.

- **Wind carry into precipitation shadows**: the rain shadow's texel cloud maps where
  precipitation lands directly, densely and accurately - and that is exactly what snow does not
  respect once the wind has it. The exposure term alone leaves every lee bare. So the flow
  window's spare channel now carries the ground wind speed per cell (from the flow grid the
  granular transport already samples), and the deposit's exposure in every pass is
  `max(exposure, groundWind * 0.8)`: blown snow settles in the lee of a wall and under an eave
  when the flow map says the wind reaches there. The transport's creep and lee-deposit terms
  move the mass; this is the fragment-level read that lets the shaders show it.

Snow on vertical surfaces stays out of the field (it is a heightfield); a windward cling term on
the mask is the cheap follow-up (section 14).

## 9. Liquids - water, ink, blood, mercury

The liquid look changes what wet and puddle MEAN on the surface: water darkens and tightens;
ink covers (opacity 0.95) with its own tint and leaves a stain that persists after the film
dries; blood rain is a translucent crimson tint (opacity 0.6) with a stain; mercury is opaque,
metallic and reflective. The stain channel is what makes a coloured rain a lasting event: a
courtyard after an ink storm stays inked until clean rain washes it or an hour passes.

## 10. The height fog layer (`SSHeightFog`, `ssheightfog.*`, replaces `sswhiteout.*`)

The whiteout pass is retired (the user's verdict: terrible and obsolete) and the fog is rewritten
as a **post-processing screen-space layer**: `SSHeightFog::getInstance()->render()` runs at the
very top of `LLPipeline::renderFinalize` (`pipeline.cpp:9344`, before the HDR/tonemap branch),
onto the linear HDR screen before tonemapping, composited with the haze blend (`BF_ONE,
BF_SOURCE_ALPHA, BF_ZERO, BF_SOURCE_ALPHA`: rgb in-scatter added, the scene and its glow
multiplied by transmittance in alpha). Drawn there it fogs everything the frame drew - alpha
surfaces, particles, clouds - by the opaque depth behind each pixel. Stated trade: a near rain
streak is fogged as if it stood where the wall behind it is; accepted for a screen-space layer.

`render()` is bound directly (like `gDeferredCoFProgram`), not through `bindDeferredShader`, in
two steps: (1) stage a depth copy (`mDepthCopy`, via `gCopyDepthProgram`) from
`gPipeline.mRT->deferredScreen` while `mRT->screen` is not yet bound at this call site - unlike
`doAtmospherics`, this pass must NOT `screen.flush()` before binding `mDepthCopy`, and it must
`screen.flush()` again itself after drawing the veil, before `renderFinalize`'s later passes
bind/flush their own targets (`mDepthCopy` and `screen` are otherwise left mismatched against
`LLRenderTarget::sBoundTarget`, and the final present at the end of `renderFinalize` draws into
`screen` instead of the back buffer); (2) bind `gSSPostFogProgram` (`ssPostFogF.glsl`) and draw
the veil onto `screen`. `gSSPostFogProgram.mFeatures.isDeferred = true`
(`llviewershadermgr.cpp:3300`), so `deferredUtil.glsl` is linked into this program automatically
(`llshadermgr.cpp:218-220`) - `ssPostFogF.glsl` only prototypes `getDepth`/`getScreenCoordinate`/
`getPositionWithNDC`/`getPositionWithDepth`, it must never redefine them (duplicate definitions
across two files of the same linked program is a link error, not a compile warning).

Per pixel the shader marches camera -> surface (32 steps, dithered start against banding)
against the surface field's column tops, so interiors stay clear, summing five demand
sources per sample (`SSScreenFX::fogDensityAt` plus the mist term, twinned against
`ssFogDensityAt` in `ssPostFogF.glsl`):

| Source | Demand (core) | Scale height | Colour |
|---|---|---|---|
| Ground fog | `groundFogDemand(humidity, wind, sun)` - humid, still, dark | `SS_FOG_GROUND_SCALE_M` 6 m (hugs the ground) | horizon colour, wisped |
| Precipitation veil | `precipVeilDemand(intensity, granular)` - heavy rain 0.25, heavy snow 0.6 | `SS_FOG_PRECIP_SCALE_M` 60 m (fills the column) | horizon colour |
| Squall whiteout | the old `squallFactor * precipitation`, still gated by `SSAtmoWhiteout` | `max(squallScaleM, 4)`, the old 10-100 m falloff ramp | horizon colour, floored |
| Drift band | the old lift demand, storm regimes only | `SSAtmoWhiteoutBand` (default 2.5 m) | horizon colour |
| Coloured mist | `liquidLook.opacity * precipVeilDemand` - outside the core's LOCKSTEP shape | `SS_FOG_GROUND_SCALE_M` 6 m, wisped with the ground term | the liquid tint (ink mist) |

Two things the old veil never had: a **wisp term** (a 3D value noise drifting with the wind
modulates the ground and mist sources only - the precipitation veil and the squall/drift band
are already visibly in motion, so wisping them on top would read as noise) and **scattering
toward the light** (a Henyey-Greenstein phase on the view-to-sun angle adds the sun colour into
the fog, so fog glows around the sun and greys away from it). Every source is relaxed CPU-side
with `SSScreenFX::relax(FOG_RAMP_IN_S, FOG_RAMP_OUT_S)` (the squall and drift sources keep their
own 8 s / 20 s ramps) so nothing fast-moving multiplies into the veil.

The sky (no surface hit, depth >= 0.99995) has no endpoint to march to: its veil integrates the
camera-height density (`ssFogSkyDensity`, computed CPU-side once per frame at the camera's own
column - above the surface field's stored column top when the camera has one, the flat
`groundZero()` reference otherwise, matching what the marched fog around the camera sees) along
the ray's climb, using the ray's AGENT-space up component (`ssFieldInvView * ray`, not the view-
space `ray.z`, which is negative for every visible pixel and would otherwise clamp the climb
distance to a constant) - so the horizon collapses into the fog while steep-up rays leave it,
same behaviour the whiteout's sky branch had.

Fog shader uniforms (`ssPostFogF.glsl`, all uploaded by hand from `SSHeightFog::render` since this
is a POST program): `depthMap`/`inv_proj`/`screen_res` (the depth reconstruction, this file's own
copies - uniforms do not cross shader files), `ssFieldInvView`, `ssFogColor`/`ssFogSunColor`/
`ssFogSunDir`, the five source dials `ssFogGround`/`ssFogPrecip`/`ssFogSquall`/`ssFogLift`/
`ssFogMist`, `ssFogSquallScale`/`ssFogBand`/`ssFogRange`, `ssFogGroundZ`/`ssFogWaterZ`,
`ssFogSkyDensity`, `ssFogWind`/`ssFogTime`, and `ssFogDebug` (S32 0/1/2: off, fog-amount
grayscale, midpoint-density x5 grayscale - both branches of `main()`, sky included, honour the
debug readout so it agrees between sky and ground pixels). `ssFieldMap`/`ssFieldOrigin` are
declared and bound by `SSSurfaceField::bindForShader`, not by this file, which only prototypes
`ssFieldFetch`. The `SSAtmoWhiteout*` settings keep their meaning for the squall/drift-band
source and the falloff/band/range dials; `SSAtmoHeightFog`, `SSAtmoHeightFogGround`,
`SSAtmoHeightFogPrecip` and `SSAtmoHeightFogDebug` are new.

## 11. Heat shimmer (`SSScreenFXPost::renderHeat`, `ssPostHeatF.glsl`)

A post pass after `combineGlow` (`pipeline.cpp:9385`, LDR, alpha is glow at this point in the
frame): the scene is re-sampled through a two-octave scrolling noise offset, weighted by depth
(nothing inside `SS_MIRAGE_NEAR_M` 6 m, full from `SS_MIRAGE_FULL_M` 60 m - LOCKSTEP against
`ssscreenfxcore.h`), by a low-angle term (the offset is strongest for rays near the horizon),
scaled by `mirageStrength`. The strength is the sum of a sunny-day **baseline** (hot air over
sunlit ground: from 24 C, full at 40 C, times sun-up) and a **thermal shock** accumulator
(`stepThermal`): a temperature RISE faster than `SHOCK_RATE_MIN_CPS` adds `SHOCK_PER_C` per
degree and decays over `SHOCK_TAU_S` (three minutes) - "heat turned up too quickly from any
temperature, which takes time to dissipate". Wind flattens it (gone at 9 m/s), rain kills it, a
narrow field of view multiplies it (up to 4x). The noise scrolls along the wind at the camera.
Uniforms: `diffuseRect`/`depthMap`/`inv_proj`/`screen_res` (depth reconstruction, matching
`deferredUtil.glsl`'s convention plus the extra `-(p.z/p.w)` to turn view z into a positive
distance - `cofF.glsl` twin), `ssHeatStrength`, `ssHeatTime`, `ssHeatWind`, `ssHeatAspect`.

## 12. Lens drops (`SSScreenFXPost::renderLens`, `ssPostLensF.glsl`)

A post pass after depth of field and before anti-aliasing (`pipeline.cpp:9400`, right after
`renderDoF` - the drops are ON the lens, so they are sharp and they blur what is behind them).
Three layers: a static-drop layer, two moving-drop layers with trails, and a condensation layer
the trails cut clear. Demand (`lensDemand`): looking up, drops accumulate and slide; looking
level, they land in proportion to how much the rain flies AT the lens and slide by gravity;
looking down, nothing. `stepLens` relaxes the wet figure in over 1.5 s and out over 6 s, snaps it
to 0 under water and pulses it to 1 the frame the camera surfaces. Condensation grows when the
lens is wet and the air is cold (full at 8 C) and is cut by the trails. Slide direction
(`slideDir`) is gravity's screen projection plus a share of the wind's - looking straight up,
gravity has no screen direction and the drops drift with the wind. Refraction samples the scene
through the combined drop normal; the rim highlight is gated by actual drop coverage and peaks
at the drop's steep rim (large normal magnitude), not its flat crown. Uniforms: `diffuseRect`,
`screen_res`, `ssLensWet`, `ssLensFog`, `ssLensTime`, `ssLensSlide`, `ssLensAspect`,
`ssLensScale`. Reflection probes in the drops are a documented follow-up (the post program would
need the class3 probe binding); the screen is the fallback the user allowed.

## 13. Settings

New `SSAtmo*` keys (F32 unless noted): `SSAtmoSurfaceDrops` (0..1, drops and drips master),
`SSAtmoSurfaceDropScale`, `SSAtmoSurfaceRings` (bool), `SSAtmoSurfaceIce` (bool),
`SSAtmoSurfaceFrost` (bool), `SSAtmoSurfaceFrostStrength`, `SSAtmoSurfaceDebug` (S32: 0 off, 1
ice, 2 frost, 3 stain, 4 age, 5 porosity), `SSAtmoHeightFog` (bool), `SSAtmoHeightFogGround`,
`SSAtmoHeightFogPrecip`, `SSAtmoHeightFogDebug` (S32: 0 off, 1 fog amount, 2 midpoint density),
`SSAtmoHeatShimmer` (bool), `SSAtmoHeatShimmerStrength`, `SSAtmoLensDrops` (bool),
`SSAtmoLensDropsStrength`, `SSAtmoLensCondensation`. The retired whiteout's own keys
(`SSAtmoWhiteout`, `SSAtmoWhiteoutStrength`, `SSAtmoWhiteoutBand`, `SSAtmoWhiteoutRange`,
`SSAtmoWhiteoutFalloff`) stay - they still gate/scale the squall and drift-band sources inside
the new layer; only `SSAtmoWhiteoutDebug` was removed (replaced by `SSAtmoHeightFogDebug`). The
prefs panel (`panel_preferences_soapstorm_atmo.xml`) gains a row per boolean; the sim floater
(`floater_ss_atmo_sim.xml`) gains the strength sliders beside the wet ones - both containers grew
enough new rows in this phase that their fixed panel heights needed raising to keep every row
reachable by scrolling (1100 and 640 respectively).

## 14. What the brief did not mention (and what this design does about it)

- **The field's accuracy is the ceiling on all of this.** The surface field is a metre-grid
  heightfield captured top-down (the rain shadow map); it knows one surface per column, so
  anything under an overhang, inside a building or on a wall is answered by the XY-only lookup
  and the per-fragment tests (the column test, the exposure march, the level and up-facing
  gates) that keep the answer from going through walls - and they do not always. Three ways up,
  none built here: (a) a **near-field tier** - a second, finer window (0.25 m cells within ~32 m
  of the camera) captured from the same stitch, so drops, rings and shorelines near the eye stop
  quantising to the metre grid; (b) a **true occupancy channel** (the WorldField's
  SOLID_VOLUME_3D / COVERAGE claims) so the exposure march reads solid vs air instead of a
  single column top; (c) **long-lived per-region analysis** - the region object cache already
  knows which objects are static for days; a region's static coverage, exposure and drainage
  could be refined offline over many visits at far higher resolution than a per-frame capture
  affords, and shipped as a per-region sidecar the field loads on arrival. User-generated content
  makes none of this exact, but (a) and (c) together make it look exact where the eye is.
- **Humidity is the missing input.** Frost and ground fog need it; the weather cube has a
  moisture figure and the shell reads it where it can, with a stated fallback. Dew (a damp
  morning film without rain) is the same input and is NOT built here - it would be a fourth
  source in the settle pass, one line, once humidity is trustworthy.
- **Drying is not uniform.** Sun and wind dry a surface faster; today `mDryRate` is flat. The
  `mSunlit` figure `stepCell` already receives is the hook; wiring it into the settle pass's dry
  rate is a one-line follow-up left out to keep the existing wet timing unchanged.
- **Wet and snow on avatars** have their own model (`ssavatarwet`); frost, ice and stain do not
  reach them. Caking already exists for snow.
- **Footsteps**: the soundscape picks puddle / wet / dry surfaces; ice (a slip, a crunch) and a
  deposit (sand crunch) are not in the picker. `SSStepSurface` needs two more entries and the
  picker two more reads of the sample - noted, not done.
- **Icicles and eave ice**: the shed cascades (runoff lips) could freeze into icicle geometry.
  Out of scope; the ice channel on the drip line is where it would start.
- **Snow on vertical faces** and **windward frost**: the field is a heightfield; a triplanar
  windward dusting in the albedo pass keyed on exposure is the cheap version.
- **Wind-driven wet streaks** on walls (rain-shadow gradients) are already the exposure march.
- **Steam off hot wet ground** after a shock (the same accumulator) - a riser particle spawn on
  wet cells when shock > 0; not built.
- **Reflection probes in lens drops** - see section 12.
- **Ice on the water plane** (freezing lakes) is the backlog's freezing system; the puddle ice
  here is its small cousin and the settings are named so the two can share a switch.
- **Melt water**: a thawing pile should produce wet - the settle pass's melt loss could credit
  `mWet`; a one-liner, left out until the thaw look is judged.

## 15. Haze altitude falloff

The stock windlight haze is **homogeneous**: `calcAtmosphericVars` (`atmosphericsFuncs.glsl`)
builds its optical depth as `density_dist = rel_pos_len * density_multiplier` (the `#else`
branch, `atmosphericsFuncs.glsl:138`) - ray length times a density constant, with no altitude
term anywhere. Nothing in that line knows how high up the camera or the fragment is. A skybox at
3 km therefore wears exactly the same haze as the ground, and washes out.

**`max_y` is not the sky's height.** It looks like an altitude - it clamps `rel_pos.y` a few
lines above the haze line, and the stock renderer also hands it to
`LLSettingsSky::getLightAttenuation(distance)` (`llsettingssky.cpp:1590`,
`light_atten = (blue_density + smear(haze_density*0.25)) * density_multiplier * distance`) as a
plain **line-integral distance** for the sunlight term. It is a calibrated column length, not a
measured height, and the ratio between it and the sky's actual dome height runs 2x-6x across
environments in this codebase (`max_y` default 1605 vs `SSAtmoEnvCloudDome::mHeightM` default
6000). Deriving the falloff's scale height from `max_y` would make the effect's strength an
accident of whatever column length an environment's author happened to pick for the sun term.

**Derivation.** The scale height instead comes from the environment's own authored cirrus/dome
height: `SSAtmoEnvApplier::apply()` samples `SSAtmoEnvCloudDome::mHeightM.valueAt(phase)` - the
deterministic keyframe, floor-relative - into `mCloudDomeHeightM` (`ssatmoenvapplier.cpp`, beside
the `mTrackFloorZ` sample), never `cirrusAltitudeMetres()` (whose own header comment,
`ssatmoenvapplier.h:79`, warns it reads the live volumetric deck and a per-client auto-detect
setting - two clients would then disagree about the very thing this feature promises is
world-consistent). A new per-environment keyframe, `SSAtmoEnvAtmosphere::mHazeThinFrac`
(`F32`, default `0.f` so every existing environment is untouched), names what share of that dome
height to use as the scale height `H`; `SSHaze::invHeight(domeHeightM, frac)`
(`sshazecore.h`) returns `1/H`, or `0` for every degenerate case (frac off, dome height off, or a
derived height under `MIN_HEIGHT_M`). Both `H` and the camera height (`hazeCamHeightM()`, floored
at `0`) are measured from `mTrackFloorZ`, so the effect is per-**track**, not per-world: a
multi-track asset with a sky track whose floor sits at 2000 m puts a camera physically at 2000 m
back at "haze height 0" (full ground haze) while that track's own dome height supplies `H` for the
ground below it - correct per-track, but test this feature on a single-track (floor 0)
environment first, or it will read as "nothing happened". The camera height is also read once per
frame off the main viewpoint (`LLViewerCamera`), so a reflection-probe or cube-map render pass
sees the *main* camera's altitude rather than its own origin - a known, presentation-only
inaccuracy, not a world-state one.

**The formula.** `SSHaze::pathFactor(camHeightM, dHeightM, invH)` is the closed-form path
integral through an exponential atmosphere `rho(h) = rho0 * exp(-h/H)` between the camera and the
fragment:

    dh = dHeightM * invH
    factor = exp(-camHeightM * invH) * (1 - exp(-dh)) / dh        (1.0 at the removable dh -> 0 singularity)

`camHeightM` is the camera's own altitude above the owning track's floor; `dHeightM` is the
fragment's altitude minus the camera's. The shader mirrors this exactly
(`atmosphericsFuncs.glsl:134-136`):

    float ss_dh = ss_world_dh * ss_haze_inv_height;
    float ss_t  = (abs(ss_dh) < 1e-4) ? 1.0 : (1.0 - exp(-ss_dh)) / ss_dh;
    float density_dist = rel_pos_len * density_multiplier * exp(-ss_haze_cam_height * ss_haze_inv_height) * ss_t;

`ss_haze_inv_height` and `ss_haze_cam_height` are two reserved uniforms
(`LLShaderMgr::SS_HAZE_INV_HEIGHT` / `SS_HAZE_CAM_HEIGHT`) uploaded from the applier's
`hazeInvHeight()` / `hazeCamHeightM()` beside `max_y` (`llsettingsvo.cpp`,
`LLSettingsVOSky::applyToUniforms`), both `0` when Atmo is inactive - which makes `ss_t` exactly
`1.0` and the whole expression bit-identical to the stock line.

`ss_world_dh` is the fragment's TRUE world altitude minus the camera's, **not** `rel_pos.y`:
`rel_pos` is a view/eye-space vector (camera-relative, but in the camera's OWN right/up/forward
axes), so `rel_pos.y` only equals world Delta-altitude when the camera happens to be level - tilt
the camera and it reads the wrong thing. A third reserved uniform,
`ss_haze_up_view` (a `vec3`), carries the world-up axis (world Z) expressed in that same
view-space frame - the same camera-axis derivation `SSVolCloud::bindGroundShadow` already uses
for its `ss_cshadow_r/u/f` ground-shadow uniforms, published here by the applier's
`hazeUpView()` (`ssatmoenvapplier.cpp`, computed from `LLViewerCamera`'s right/up/forward axes'
world-Z components: `(right.z, up.z, -forward.z)`). `atmosphericsFuncs.glsl` then reads
`ss_world_dh = dot(rel_pos, ss_haze_up_view)` before anything else touches `rel_pos`.

**Shares of stock haze**, cirrus 6000 m and frac 0.5 (`H = 3000`):

| from -> to | share |
|---|---|
| ground -> ground | 100% |
| ground -> 1 km | 85% |
| ground -> 3 km | 63% |
| ground -> 6 km | 43% |
| 3 km -> 3 km (level) | 37% |
| 3 km -> ground | 63% |
| 6 km -> 6 km (level) | 14% |

**Symmetric by construction**: `shareOfGround(fromM, toM, invH)` (`pathFactor` under the hood)
gives the same answer for `(a, b)` as for `(b, a)` - the same column of air sits between two
points whichever end the camera is at, so a sky build reads equally hazed from above or below
(and the table above is the same read both directions: ground -> 3 km and 3 km -> ground both
63%).

**In-scatter follows for free.** The falloff multiplies the optical depth only; the shader turns
that into a transmittance (`combined_haze = exp(-combined_haze * density_dist *
distance_multiplier)`) and then the airlight (`additive *= vec3(1.0 - combined_haze);`,
`atmosphericsFuncs.glsl:201`) - so a thinner optical depth automatically dims the glow/airlight
term too, without a second, independent falloff to keep in step.

**The `max_y` ray clamp stays, unconditionally.** An earlier draft skipped the stock clamp
(`if (abs(rel_pos.y) > max_y) rel_pos *= (max_y / rel_pos.y);`, `atmosphericsFuncs.glsl:96`)
whenever the falloff was live, on the theory that the falloff's own integral already converges.
That re-baselines the haze against a ray length the shares table above never assumed - on a
`max_y = 1000` environment it made a 2 km skybox look *hazier* looking down at the ground than
stock, the opposite of the intended effect - because the clamp is not a second altitude term, it
is a hard ray-LENGTH divider, and the falloff scales density; they are different quantities. The
fix keeps the clamp exactly as stock leaves it (including its direction flip for very steep
views, which `haze_glow`'s sign depends on) and captures `ss_world_dh` from the *pre-clamp*
`rel_pos` (`atmosphericsFuncs.glsl:92`, ahead of the clamp at `:96`), so the clamp still bounds
ray length precisely like stock while the falloff still reads the fragment's real altitude. The
sky dome and the dome clouds hold their own copies of this same clamp for their own geometry
(`skyV.glsl:152`, `cloudsV.glsl:160`) - this change is scene-geometry haze only, and does not
touch either dome or the volumetric deck (`ssVolCloudV.glsl` builds its own `slab_len` entirely on
its own uniforms and never reads `ss_haze_*`) - so with the falloff live, expect a visible
disagreement at the terrain/sky-dome horizon and around cloud puffs, since only the terrain/object
side of that junction is thinned. Water goes through the same shared function
(`calcAtmosphericVarsLinear`) but with a deliberately *folded* drawn position
(`waterV.glsl:106-107`, so a sky build's ocean does not fog by its true kilometres) - the falloff
now applies on top of that fold, so an ocean seen from altitude will read noticeably clearer than
its hand-calibrated stock look; re-check the water fog look once this is on.

**Retuning caveat.** Turning the falloff on for an environment thins the haze everywhere above the
scale height, not just at extreme altitude - a build a few hundred metres up already reads
measurably clearer. An environment that liked its stock haze at ground level may want
`haze_density` raised slightly once `mHazeThinFrac` is turned on, to keep the ground-level look
unchanged while the falloff does its job higher up.

**Editor.** The Atmo env editor's atmosphere/look panel gets one row beside the existing haze
rows: "Haze thins with altitude", a 0..1 slider on `mHazeThinFrac` (tooltip names the effect, `0`
= off), with a read-out of the resulting scale height in metres via `SSHaze::invHeight`.

## 16. Test ladder (`V:\Scratch\surface`)

The same runner as the atmo harness, pointed at this worktree: `node V:\Scratch\surface\test.js
[filter] [--changed] [--for header]`. Rungs: `tests/surfacestatecore.cpp` and
`tests/screenfxcore.cpp` (unit: every invariant in the contracts, golden pins at seed
0x5EED1337), `tests/scenario_*.cpp` (a cold night: rain -> freeze -> thaw; a humid still night:
frost forms, morning sun clears it; ink storm then clean rain: stain sets and washes; ash then
rain: wash; two clients at 30 and 144 fps stepping the same fixed quanta bit-identically),
`tests/twin_*.cpp` (the GLSL laws transliterated from the shader text: porosity, wet exponent,
drop law, deposit laws, ring, fog density; LOCKSTEP constants read from both sources),
`tests/visual_*.cpp` (PPM/PNG: state timelines against temperature, the drop-law curves, the
ring packet at four ages, the fog profile, the lens demand over pitch x rain angle, the shock
decay, palette swatches under wet/ice/frost).

## 17. Phases

1. Cores + tests (this doc's sections 2, 4-8, 10-12 laws) - harness green.
2. Field shell: channels, state window, looks and mixes, presets and built-ins, ring buffer,
   impact routing; shader include + the three passes; wiring (shader manager, settings, prefs,
   pipeline). Twins. Opus GLSL-by-inspection review (no shader compiler on this machine).
3. Height fog rewrite as a post pass with its sources; screen FX singleton, two more post passes, wiring. Twins. Review.
4. User build gate: what to look at is listed in the phase report.
