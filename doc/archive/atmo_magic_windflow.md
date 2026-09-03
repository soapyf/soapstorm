# Atmo Magic: wind flowmap

> **OUT OF DATE** — archived 2026-08-26. Kept for historical reference; the code is the ground truth.

The weather parameters carry a single wind vector. That is fine for an open
field and wrong everywhere else: a courtyard is calm, an alley lined up with the
wind is *windier* than open ground, the lee of a building is a dead pocket, and
a rooftop is windier than the street beneath it.

The flowmap solves for what the wind is actually doing in the build around you,
and feeds the ambient audio mix and precipitation advection.

## Requirements

Compute shaders, so **OpenGL 4.3**. Below that the flowmap never builds and
every consumer falls back to the uniform ambient wind — the audio mix reverts to
its raycast-probe classification, and precipitation drifts uniformly as before.

Adding compute support meant a small change to `LLShaderMgr::loadShaderFile`
(`indra/llrender/llshadermgr.cpp`): `GL_COMPUTE_SHADER` now pins `#version 430`
and emits `COMPUTE_SHADER` rather than claiming to be a vertex stage. Everything
else in `LLGLSLShader` was already generic over shader type.

## How it is built

### 1. Domain

**One tile per region**, anchored to the region, cached and lazily refreshed —
the same shape as the rain shadow maps. A tile covers its region plus
`SSAtmoWindFlowMargin` metres (64) of overlap into each neighbour, at
`SSAtmoWindFlowCell` metre cells (4, about the width of an alley), which for a
standard 256 m region is a 384 m domain at 96 texels.
`SSAtmoWindFlowRes` is a ceiling on that, not the resolution itself; only a wide
varregion reaches it, and when it does the cells get coarser rather than the
solve running away.

Everything horizontal is stored **region-local**, so a tile survives the
agent-origin shift on a region crossing without being touched. Altitudes are
absolute and do not shift.

The overlap is what answers the seam problem. Solving each region in isolation
would leave wind stopping dead at a sim border; a tile that can see 64 m into
next door produces very nearly the same flow near the border as its neighbour's
tile does, because both are looking at the same buildings.

This replaced a single camera-centred box. That box re-centred whenever the
camera crossed one cell — 4 m — which meant a capture, a GPU solve and a
synchronous readback roughly once a second at walking pace, and it made the
whole field crawl along behind you.

### 2. Height capture, then oblique probes

Five orthographic depth renders through `LLPipeline::renderShadow`.

**One overhead pass** gives the highest surface of every column, and everything
below it starts out solid. On its own that is right for a building — solid from
grade to roof, walls and interior included — and wrong under a skyway, which
reads as a solid block down to the street.

**Four oblique probes**, one per cardinal direction, tilted down by
`SSAtmoWindFlowProbeAngle` (30° by default), then carve that answer back. A
point nearer to a probe than that probe's first hit — where that probe *hit
something* — has a clear line to a surface beyond it, so it is air.

The hit requirement is load-bearing. A probe ray that leaves the captured
volume without meeting anything says nothing about the points along it: the
capture is bounded by a frustum and by whatever was loaded, so a miss is the
absence of evidence rather than evidence of absence. Reading a miss as air is
fatal because the probes are OR'd — roughly 87% of each probe image is
legitimately sky, so nearly every cell lands on a miss in at least one of the
four directions, and one would then be enough to open it. Nothing is lost by
being strict: a passage is discovered from a ray that enters one end and hits
something past the far end, which is a real hit at a greater distance. The test is deliberately
one-sided: evidence can only ever turn solid into air, never the reverse. A
passage the probes fail to see keeps the conservative heightmap answer instead
of becoming a hole that is not there, and a probe ray stops at the first
surface it meets, so a half-metre wall shadows everything behind it exactly as
completely as a thick one.

That asymmetry is the whole design. Earlier attempts inferred openness from a
second *surface* per column — the lowest downward-facing one, or the soffit of
the topmost structure — and both fail, in opposite directions, for the same
reason: a vertical depth capture cannot see vertical surfaces at all. Taking
the lowest underside puts a prim street's soffit above a skyway's, so the
underpass fills in. Taking the topmost soffit erases every wall under a roof,
however thick, so buildings hollow out and leak. The two rules are exact duals
and there is no third choice between them, because a room and an underpass are
the same shape — a void with a floor and a ceiling — distinguished only by the
vertical surfaces neither rule can see.

A visual hull over six directions fails differently: it is an *intersection*,
so each additional building adds occlusion, and in a dense grid a cell under a
skyway in a street canyon is occluded from all six directions and fills back
in. Carving is a union, so density never works against it.

The probes also produce the altitudes the slicer needs. A probe hit landing
more than `HIDDEN_CLEARANCE` below the overhead surface for its column is by
definition something the overhead pass could not see — the underside of an
overhang, or the ground beneath it — and both are altitudes a slab boundary has
to land on for the passage to have anywhere to exist.

Because these are *renders*, non-physical geometry is captured correctly —
including 1024 m sim-surround megaprims, which a physics raycast would miss
entirely.

### 3. The band

A tile solves one horizontal band, not the whole 4096 m column. Doing it inside
a band is what stops a skybox 1000 m up from stretching the slices into
uselessness at ground level.

The band is chosen from the **sky track the camera is in**, not from its
altitude, which is what lets the tile be static: moving around inside a track
does not move the band.

- **Base** — at ground level, the lowest land in the region, or the water
  surface where the seabed drops below it, so a deep trench does not drag the
  band away from the build. In a sky track, the track's own floor.
- **Ceiling** — probed. `SSAtmoWindFlowHeight` (192 m) is only a starting guess
  at how tall the build is; the top-down capture actually looks down from four
  times that and settles the ceiling on the 99.5th percentile of the surfaces it
  found, plus headroom. A high percentile rather than the maximum, so one lone
  platform on a pole does not stretch the band while a genuine 400 m tower still
  fits inside it.
- The band never crosses a track ceiling, because a slab must not straddle two
  tracks.

### 4. Adaptive slicing

The band is split into 2–8 slabs.

- **Count** from the captured height range over `SSAtmoWindFlowSliceMin` (4 m),
  capped by `SSAtmoWindFlowSliceMax`.
- **Placement** at quantiles of a height histogram, so boundaries land where the
  geometry actually changes rather than spreading evenly through empty air.
  Content is heavily bottom-weighted — streets, doorways and alleys all live in
  the first ~20 m.
- **Forced boundaries** at any EEP track altitude inside the band, so a slab
  never straddles two tracks. Each slab then carries exactly one ambient wind,
  which is what lets per-track weather and the flowmap compose.

### 5. Solve

A 3D-coupled pressure projection, all on the GPU:

| Pass | Shader |
|---|---|
| solid mask + seed velocity | `ssWindInitC.glsl` |
| divergence | `ssWindDivC.glsl` |
| pressure (×`SSAtmoWindFlowIterations`) | `ssWindJacobiC.glsl` |
| subtract gradient + exposure | `ssWindProjectC.glsl` |

Each Jacobi iteration dispatches over the **whole 3D domain in one call**, so
the slab count is not the performance axis — 8 slabs costs the same number of
dispatches as 1, just more threads per dispatch.

`SSAtmoWindFlowIterations` is the dial that decides whether the wind actually
notices buildings. Jacobi carries pressure information **one cell per pass**, so
a domain 96 or 192 cells across needs passes of that order before the field has
heard about the far side of it. Undershoot and the solve returns something close
to the uniform inflow with a slight dent near obstacles. Now that a tile is
solved once per region and then left alone, the headroom is cheap: a few hundred
passes over ~55k cells is milliseconds.

Coupling the slabs vertically is what makes air blocked at street level actually
go up and over a roofline; independent stacked 2D solves would simply lose that
mass. Vertical velocity falls out of the solve rather than being synthesised
from a height gradient.

Slab spacing is non-uniform by construction, so every vertical difference is
weighted by the real slab thickness.

**Boundaries:** horizontal edges and the top slab are open (ambient pressure,
air free to enter and leave); the ground is a wall; a solid neighbour mirrors
the reading cell's own pressure, which puts a zero gradient across that face so
nothing crosses it.

That last one is the whole reason a building deflects anything, and it is worth
stating plainly because getting it subtly wrong looks like a tuning problem
rather than a bug. Reading the pressure *stored in* a solid cell instead holds
every wall at zero — an open drain. Air pours into the geometry, no pressure
builds against a windward face, and the field barely bends no matter how fine
the grid or how many passes it gets. The same rule has to be applied again when
the gradient is subtracted, or the projection pushes the air straight back into
the walls the solve just steered it around.

**Lee sheltering** is a short upwind march in the project pass. Projection alone
has no memory, so it slows air in front of an obstacle and squeezes it through
gaps but never produces the calm pocket behind a building. `SSAtmoWindFlowShelterSteps`
controls the reach; 0 disables it.

### 6. Atmospheric gradient

Ground drag slows air near the surface and releases it with height. The ambient
wind for each slab is scaled by a power law against a 10 m reference,
`SSAtmoWindFlowGradient` (0.25 by default; ~0.4 for a dense city, 0 for a
uniform wind at all altitudes).

This is why getting above the roofline should feel exposed, and it is measured
from the *low* end of the captured surfaces rather than the mean — the mean
would be dragged upward by the very rooftops the gradient is measuring from.

### 7. Travelling gusts

The solve is static, and a storm built out of it alone blows at one unvarying
strength forever, everywhere at once. The rhythm is layered on at sample time
instead, in `SSWindFlowMap::gustAt()`, as *frozen turbulence*: a deterministic
noise field fixed in the moving air rather than in the world.

Sampling point `p` at time `t` projects `p`'s grid coordinates onto the wind
axis and subtracts how far the air has travelled:

```
along  = dot(p_xy, wind_dir) - drift      drift = SSAtmoMagic::windDrift()
across = cross(p_xy, wind_dir)
gust   = 1 + depth * (0.8 * fbm2(along/L, across/2.4L) + 0.3 * value2(along/0.27L, across/0.65L))
```

That subtraction is the whole trick. Standing still, the pattern is carried past
at the wind's own speed and the wind rises and falls as it goes by; watched from
above, a surge enters on the windward side of the region and walks across it,
reaching the far edge as much later as the air takes to get there. Nothing about
it is a clock — it is one field, and where you are in it is where the air has
carried it to. Fronts arrive every `gust_length / windspeed` seconds: 140 m in
a 14 m/s wind is a wave every ten seconds.

Depth, spacing and veer are the track's own weather, edited on the main Atmo
Magic window next to turbulence and wind speed and serialized into the parcel
notecard (`gust_depth`, `gust_length`, `gust_veer` — see
[atmo_magic_tracks.md](atmo_magic_tracks.md)). Only `SSAtmoWindGustTravel`, how
fast a front moves relative to the air, is a viewer-side dial.

Details that matter:

- **Anisotropy.** The field is long across the wind and short along it, because
  a gust front arrives as a line, not a blob.
- **Veer.** A third channel swings the wind a few degrees either side of its
  mean as the front goes through (`gust_veer`). A gust that only
  changed speed reads as the whole world pulsing rather than as weather.
- **Asymmetry.** Peaks are stretched and troughs compressed, so the wind spends
  longer in the lulls between gusts than inside them.
- **Shelter.** The modulation is scaled by the cell's exposure, so a courtyard
  the wind barely reaches feels the surges as a distant swell rather than as the
  front itself.
- **Depth follows turbulence.** The track's turbulence parameter scales its
  gust depth, so a calm track still gets a steady draught however deep the
  gusts are set, and the crossfade eases both, so crossing a band boundary
  cannot step from still air into a squall between frames.
- **Drift, not `speed * time`.** `windDrift()` integrates `|wind| * dt`. Phasing
  off speed times the clock instead would shift the entire field bodily every
  time the wind eased to a different speed. It is seeded from the shared clock,
  so two clients standing in the same steady wind agree on where in the cycle
  they are, and wraps at 2^20 m — one phase jump per several hours of hard wind.
- **Slow evolution.** A real eddy changes as it travels; a crosswind slide on
  the shared clock decorrelates the field over a couple of minutes so a steady
  wind does not visibly repeat itself.

`sample()` applies the gust to whatever it is about to return, including the
ambient fallback outside a solved tile, so the wind does not surge on one side
of a tile border and hold steady on the other. Every consumer — precipitation
advection, in-world particles, the wind audio bed — gets the rhythm for free and
gets it in step.

## Output and consumers

One `RGBA16F` volume: `RG B` = velocity m/s, `A` = exposure (how much of the
ambient wind reaches this cell; below 1 is sheltered, above 1 is a gap the wind
is being squeezed through). Read back once per rebuild for CPU consumers.

A tile is solved once and then left alone. It is rebuilt when:

- geometry inside its domain moves (AABB-tested, so a megaprim centred in the
  next region over still counts). A region rezzing in fires object updates
  continuously, so a dirty tile waits at least 3 s between solves and settles
  once things stop moving.
- the ambient wind changes beyond a threshold. The map is only static with
  respect to a fixed inflow.
- the camera moves to a different sky track, so the band is no longer the one
  being flown in.
- the cell size or margin settings change.

At most one region is solved per frame, throttled, and at most four tiles are
cached — the camera's region, plus neighbours once the camera is within 96 m of
their border.

- **Audio** (`ssweathersounds.cpp`) — wind loop gain now rides the locally
  solved speed and exposure instead of the stepped `mOutdoorSize` probe
  classification. Continuous, and it distinguishes an alley aligned with the
  wind from one across it. Roof beds still use the raycast cover probe; cover is
  a different question from flow.
- **Precipitation** (`ssprecipitation.cpp`) — `windAt()` samples the flowmap, so
  drops curve around buildings and funnel down alleys. Toggle with
  `SSAtmoWindFlowAdvect`.
- **In-world particles** (`llviewerpartsim.cpp`) — anything flagged
  `LL_PART_WIND_MASK` takes its wind from the flowmap instead of
  `LLViewerRegion::mWind`, so scripted smoke, dust, leaves and steam blow the
  way the rain does. This is a **replacement, not a blend**: while Atmo Magic is
  running and the camera's region has a solved tile, the sim's wind field is not
  consulted at all — averaging the two gives a third field that is neither, and
  the first thing it loses is the courtyard the solve says is still. Calm
  weather is not a handover either; when the weather says the air is still, the
  air is still. Toggle with `SSAtmoWindFlowParticles`.

  The rate a particle takes up that wind had to move with it. The viewer settles
  a wind-tagged particle onto the wind at a tenth per second — a ten second time
  constant — which is fine against a region-wide breeze, since the whole region
  *is* that one wind and being slow to reach it costs only a lag. A solved field
  is the opposite: alley mouths, rooftop updrafts and the lee behind a wall are
  metres across and seconds apart, so a particle smoothing over ten seconds of
  them feels their average, which is the uniform breeze the solve was supposed
  to replace. `SSAtmoWindFlowParticleResponse` multiplies that rate while the
  flowmap is driving, defaulting to 4 (a 2.5 s constant); at 1 the viewer's own
  behaviour is back. The take-up and the drag are the same number either way, so
  a particle still settles at the speed of the air around it and no faster.

- **`gWindVec`** (`llappviewer.cpp`) — the viewer's global wind vector, which is
  what the wind heard in the headphones is built from. It now samples the
  flowmap at the avatar rather than the region field, so the wind you hear is
  the one blowing down the alley you are standing in. The **water** is
  deliberately left on a region-scale wind — `gSky.setWind()` takes the
  undisturbed weather wind, because the local field around one build says
  nothing about how the sea beyond it should run.
- **Flexible prims** (`llflexibleobject.cpp`) — a hanging banner in an alley
  feels the draught coming down it. Sampled **once per prim**, not per section:
  the field's cells are metres across and a flexi is a couple of metres end to
  end, so its sections would be reading one cell between them anyway, and this
  runs for every flexi in sight every frame.

Both are behind `SSAtmoWindFlowViewerWind`. With that off, or with no solved
tile under the camera, they fall back to `LLViewerRegion::mWind` exactly as
before.

`SSWindFlowMap::drivesWind()` is the single gate all of these ask — Atmo Magic
running, and a tile solved under the camera — so there is one answer to "is the
flowmap the wind right now" rather than four subtly different ones.

This does not introduce a new cross-client divergence class: impacts already
depend on local geometry through the rain shadow map. The deterministic streams
(spawn seeds, gust envelope, area field) are untouched.

## Debugging

**Atmo Magic → Simulation...** holds every dial below plus explicit rebuild
buttons for both maps, live status for each, and the debug view detail settings.
Changing anything there triggers a rebuild, because both maps are built once and
would otherwise keep showing the old answer.

**Develop → Render Metadata → Wind Flow (Atmo Magic)**

One arrow per solved cell, on every slab, at the density the solve actually ran
at — nothing is interpolated or scaled up, an arrow sits at a cell centre and
reads exactly one texel.

- **direction** from the arrow, with a dark tail fading to a bright head. The
  gradient is what makes direction legible without barbs, which is what lets
  the density go up by a factor of sixteen for the same cost.
- **hue** also encodes direction, so a whole slab reads at a glance and
  opposing flows come out as opposing colours
- **length** carries speed relative to the domain's own ambient wind, so a
  venturi through an alley overruns its cell and a lee pocket shrinks to a stub
- **head brightness** carries exposure

Full texel density out to `SSAtmoWindFlowDebugRange` from the camera, then
decimating 2× and 4× beyond it, so the far side of a 768 m domain still costs
something sane. A thin box outlines each tile's domain, showing which
region it belongs to and how far its margin reaches into the neighbours it
overlaps.

This replaced a translucent slab-quad view. Those quads were a 48×48 subsample
of a 192×192 field blown up to sixteen times the area, so every quad was a
single point sample pretending to cover its neighbours — which is exactly why
it never looked like the underlying data.

The Atmo Magic info overlay (`SSAtmoShowInfo`) gains a `-- wind flow --` section
with the domain, tile count, slab altitudes, local wind vector, exposure, build
time, how long ago the tile under you was solved, and a **build count**. The map
is meant to be static, so a count that climbs while you stand still means
something is churning.

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `SSAtmoWindFlow` | on | Master toggle |
| `SSAtmoWindFlowCell` | 4.0 | Target cell size, metres |
| `SSAtmoWindFlowMargin` | 64 | Overlap into each neighbouring region, metres |
| `SSAtmoWindFlowHeight` | 192 | Nominal band height; the capture probes 4x this and settles it |
| `SSAtmoWindFlowRes` | 192 | Ceiling on texels per axis |
| `SSAtmoWindFlowIterations` | 128 | Pressure passes; the main quality dial |
| `SSAtmoWindFlowSliceMin` | 4.0 | Minimum slab separation, metres |
| `SSAtmoWindFlowSliceMax` | 6 | Slab cap, 2–8 |
| `SSAtmoWindFlowShelterSteps` | 6 | Upwind reach for lee sheltering |
| `SSAtmoWindFlowGradient` | 0.25 | Boundary-layer exponent |
| `SSAtmoWindFlowAdvect` | on | Let the flowmap carry precipitation |
| `SSAtmoWindFlowParticles` | on | Let the flowmap drive wind-tagged in-world particles |
| `SSAtmoWindFlowViewerWind` | on | Let the flowmap drive the wind audio bed and flexible prims |
| `SSAtmoWindFlowParticleResponse` | 4.0 | How fast those particles take up the local wind, against the viewer's own rate |
| `SSAtmoWindFlowDebugRange` | 128 | Debug arrows at full texel density within this range |
| `SSAtmoWindGustTravel` | 1.0 | Front speed relative to the air; 0 pins the pattern to the ground |

## Known limitations

- **Two-layer capture.** One solid span per column. Multi-storey overhangs (an
  arcade under a building under another overhang) degrade to a single span
  rather than being resolved exactly.
- **LOD and asset timing.** The capture renders whatever LOD is currently
  resident. A mega sculpty centred far away can sit at low detail while filling
  the domain, and a capture taken before its sculpt map has fetched bakes the
  placeholder silhouette until the next rebuild.
- **Synchronous readback.** The solved volume comes back with `glGetTexImage`.
  Rebuilds are rare enough that this is cheaper than the machinery to avoid it,
  but it is a stall on the rebuild frame.
- **Jacobi.** A plain Jacobi relaxation, not multigrid, so convergence is
  linear in the pass count rather than the near-constant a multigrid solve would
  give. That is what makes the pass count a tuning problem instead of an
  implementation detail.
- **Gusts do not re-solve.** The rhythm scales and veers the solved field; it
  does not re-run the pressure solve for the gusted wind, so a gust does not
  find a new path through the build, only a stronger one through the same one.
- **Band height.** A build taller than the probe range (four times
  `SSAtmoWindFlowHeight`, so 768 m by default) is clipped at the ceiling and
  reads as solid above it.
