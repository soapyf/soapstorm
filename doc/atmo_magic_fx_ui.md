# Atmo Magic: the Effects & LOD floater

`floater_ss_atmo_fx.xml`, registered as `ss_atmo_fx` in `llviewerfloaterreg.cpp`, class
`SSFloaterEffects` (`ssfloaterfx.cpp`). Opened from the Atmo Magic floater's own button
(`ssfloateratmo.cpp:45`).

## Why tabs

It was one column, 1008 pixels tall, `can_resize="false"`, with no scroll. On a 1080p
screen with a taskbar the lightning half was off the bottom edge and unreachable — the
floater was taller than the room it had, and nothing in its markup let a person do
anything about that. It had also collected two elements both named `impact_label`, which
is what happens when sections are appended to a list rather than placed in a structure.

Four tabs, split by **what a person is doing**, not by which subsystem owns the setting:

| Tab | Holds | Why together |
| --- | --- | --- |
| Rain | Density, particle budget, impact toggles, drop opacity, ripple size/speed/opacity, the water shader switch, glow master, drop roundness, streak contrast, sparkle, the column trace debug, the rain shadow and roof runoff overlay toggles | Tuning rain is one continuous act. You change the density, then the opacity, then the splash that the new density made too loud. Splitting that across tabs means paging back and forth mid-adjustment. |
| Lightning | Enable, strike triggers, pending-strike markers, 33 dials (the bolt's, then the ground strike's: amber zone, bead, plasma, late restrike, aura, flare, fire, crawl, sparks - `doc/atmo_magic_lightning_strike.md`), seasonal charge, bolt from the blue, hidden-ground-show skip, bolt texture | Nobody tunes lightning while tuning rain. This block alone is ~490 lines of markup and is the reason the floater outgrew one column. |
| Clouds | The volumetric field's three switches and the debug overlay | New tab. See below. |
| LOD | Precipitation's three distance tiers, the cloud field's density and puff budget | The one place to go when the weather costs more than the machine has. These are *how much to draw* decisions, and they belong together rather than each beside the look dials of the system it trims. |

Each tab is a `scroll_container` wrapping a fixed-height content panel, so the floater is
`can_resize="true"` and a tab taller than the window scrolls instead of being cut off.
Content rows kept their original geometry (`left="12"`, 368px wide); the floater widened
from 380 to 424 to leave room for the tab border and the scrollbar gutter.

## The Clouds tab

### The three switches were unreachable

`SSAtmoVolumetricClouds`, `SSAtmoCloudProceduralNoise` and `SSAtmoCloudTessellation` were
declared only by lazy `LLCachedControl` constructions inside `ssvolcloud.cpp`. That has two
consequences nobody wants:

- **They did not persist.** A control `LLCachedControl` declares itself gets no `Persist`
  flag, so every setting reverted on restart.
- **They did not exist until used.** The declaration happens on first execution of the
  function holding the static, so `SSAtmoCloudTessellation` was absent from Debug Settings
  until the cloud renderer had drawn at least one frame.

They are real `settings.xml` entries now, and they have a home in the UI.

### Tessellation

The toggle used to subdivide the puff's own card — first as `segs = tessellate ? 4 : 1`, then
by on-screen size, with `skirt = puff.mAnvil * v²` shearing the top rows into an anvil skirt
and `SSAtmoCloudEdgeBreakup` wandering the rim vertices. Two things killed it:

- **It boiled in the wind.** The rim displacement was seeded by quantising the puff's *world*
  position. The field's positions include the live wind drift, so every metre the wind pushed
  a cloud re-rolled the seed and the carved rim churned — the pattern was anchored to the
  world, not to the cloud it decorated.
- **It modified the puff it was supposed to serve.** The rim breakup and the skirt both moved
  the original card's vertices, so toggling the feature changed what every puff already was
  rather than adding anything to the field.

It is a **puff refinement LOD** now, built in `SSVolCloud::buildDeck` where every other puff
is built:

- **The original puff is untouched.** The render pass emits every puff as the single
  camera-facing card it always was; there is no per-vertex displacement anywhere in the path.
- **It adds smaller and smaller puffs.** A placed puff within `SS_TESS_RANGE_M` (1000 m) of
  the eye grows `SS_TESS_CHILDREN` (4) children at a fraction of its radius, scattered
  around its body; within `SS_TESS_INNER_M` (500 m) those children grow a smaller generation
  still (`SS_TESS_GRANDCHILDREN` = 3). Detail scales down geometrically as it climbs toward
  the viewer, and only exists where cloud already does — children hang off existing parents,
  so the coverage gate's holes stay holes.
- **It is stable and moves with the wind.** Every child offset is hashed off the parent's
  AIR-FRAME cell — the same anchor the parent's own jitter uses, with the drift left out — so
  a child holds its place inside its cloud while the wind carries both. This is the same
  air-frame discipline the fragment carving already runs on.
- **The LOD is a distance fade, not a count switch.** Each ring's puffs fade out approaching
  their limit (`smoothstep` from half the range to the range, applied to the parent's
  distance), so walking toward a cloud gathers detail incrementally and no ring boundary ever
  pops.
- **The budget pays for it.** Children are ordinary `Puff`s: same sort, same
  `SSAtmoCloudPuffBudget`, same fragment carve. They are the nearest bodies in the deck, so a
  full budget trims the field's far edge to make room — the LOD trade that dial already was.

The anvil skirt is gone with the rest of the vertex work; the anvil itself is untouched — it
is carried by the fragment-stage carving (lid flattening, tower cut, cap band) and by the
builder's width shaping, neither of which ever needed the tessellation toggle.

`SSAtmoCloudEdgeBreakup` no longer exists.

**Still not structure-aware.** Nothing in the cloud render path knows where buildings or
terrain are. The only proximity handling is `ss_soft_m` in `ssVolCloudF.glsl`, a
fragment-stage alpha fade against the depth copy so a puff thins as it approaches a surface
instead of ending on a hard edge. Doing that in geometry would mean vertex-stage reads of
the depth copy at billboard corners.

### Field density and budget

Both live on the LOD tab, because both are *how much to draw* decisions.

- **Field density** (`SSAtmoCloudPuffsPerCell`, default 3, range 1-8) is how many puffs the
  builder places in each 260m cell it keeps. Sub-puff index 0 is always placed, so lowering
  it thins the field toward one body per cell rather than opening holes in it — holes are
  the coverage gate's job and the two must not be confused. Past the squash knee the loop
  already collapses to one puff per cell, so this is a near-field cost buying near-field
  body.

  `CELL_M` itself is deliberately **not** exposed. `ssVolCloudF.glsl` replicates the cell
  grid verbatim (`SS_CELL_M = 260.0`) to run the builder's gate per fragment, so moving the
  grid would desync the carving from the geometry it carves. The sub-count is the builder's
  alone and nothing downstream replicates it.

- **Puff budget** (`SSAtmoCloudPuffBudget`) caps how many puffs a deck may draw, replacing
  what was a hardcoded `MAX_PUFFS = 1260`. Applied **per deck**, after the depth sort, so
  the erase takes the field's far edge and leaves the sky directly overhead whole — the same
  shape as precipitation's distance tiers. A sky with an under deck draws up to twice the
  budget. Clamped to `[64, 8000]`.

There is **no viewer-side per-puff opacity dial**. `mPuffDensity` is the sky's own authored
figure and belongs to the environment editor; a multiplier on top of it here would just be a
second place to set the same thing.

## The debug overlay

`LLPipeline::RENDER_DEBUG_CLOUD_FIELD`, dispatched from `pipeline.cpp` to
`SSVolCloud::renderDebug()`. Reachable from the Debug floater's Overlays tab, which
now holds every Atmo Magic overlay switch in one place - the copies that used to
sit on this tab, the Simulation floater and the Develop → Render Metadata menu are
consolidated there, and the floater's shell class went with them since every
remaining control binds itself through `control_name`.

`SSAtmoCloudDebugView` picks what it draws. Both views also get both decks' bands: the
floor and lid each field was built between, as a ring at the field's fade radius.

| View | Shows | Answers |
| --- | --- | --- |
| 1 Cells and towers | The builder's 260m air-frame grid replayed across the field: green where the gate passed and cloud was placed, red where it did not, with a stalk as tall as the noise map's tower weight wants that column to climb | Where the holes come from, and whether the map's convection geography is the shape you meant |
| 2 Column profiles | For cells near the camera that the gate kept, the column that cell grows, outlined at its true position and altitude. Half-width at each height is exactly what the builder gives a puff there — waist, flare, the profile ramp's anvil term and the deck-wide one, replayed off the deck rather than approximated. Hue is the anvil figure in force at that height, so the altitude the ramp takes over at reads as the colour changing partway up | What the profile ramp actually does to a cloud's shape, standing beside the cloud it shaped |

Both views walk cells rather than puffs. Per-puff quad outlines were tried and cut: at ~2500
puffs they occlude the sky they are describing, and a rectangle per puff says nothing a
person could not already see. The cell grid is the register the field is actually authored
in, and it is legible.

View 2 uses a small neighbourhood (11x11 cells) on purpose. A column outline is a legible
thing and a thousand of them are not; whole-field geography is view 1's job.

### The squash

Every mark goes through `squashScale()`, the same far-field compression
`ssVolCloudV.glsl` applies to the field itself. Without it two things break: the marks land
kilometres behind the cloud they describe, and the far half of a 5km field never survives a
2km far plane to be drawn at all. Lines are subdivided *before* they are squashed, because
the squash is not linear along a segment and the band rings span kilometres.

## The Rain tab's column trace

`SSAtmoRainTraceDebug`, a checkbox on the Debug floater's Rain tab and a setting rather than a
debug mask — the same arrangement as the celestial overlay. It is an authoring aid for the pane
beside it ("why is the ground dry under this roof"), not a view of a capture, so it
did not belong in the Simulation floater's shadow-view combo or behind a debug mask bit.

`SSRainShadowMap::renderColumnTrace()` (`pipeline.cpp` dispatches it) walks the drops tier's
**spawn grid** — the same 8m world cells `spawnTierCell` samples — so every line is a column
rain actually tests, not a second grid that could disagree with the one the spawner uses.
Each line climbs from the ground the column would land on, along the fall direction,
toward the weather source (the deck's base; a fall length above the ground when no deck is
built), and the colour is the answer:

| Colour | Meaning |
| --- | --- |
| Cyan | Open to the source: rain reaches the ground here |
| Red | Something shelters the column; the line stops at the shelter, which catches the rain while the ground below stays dry |
| Violet | The landing sits above the weather deck's top — no weather source exists over it, and the spawner rejects those columns outright |
| Amber | The capture never mapped the column, so nothing is known; drawn as a stub because a full line would claim a sky it cannot see |

The violet case exists because red would misreport it. A roof past the deck top is *dry* —
red says "the shelter catches the rain", and up there nothing catches anything.

Open columns thin out over the last 10–30m above their impact, so the landing stays
readable where the ripples and splashes it feeds draw. Density steps with distance — every
column inside 96m, every 2nd inside 192m, every 4th to the 256m edge — with the parity
taken on the world cell indices so a line keeps its slot as the camera pans. The stride
bands and the radius are fixed constants: the shelter answer is local, and a dial would
invite reading weather into columns the camera cannot judge. Ground-risen weather (riser
presets) draws the riser band instead of a climb to the deck, since nothing falls from the
deck for it — an open column is where the weather appears at grade.

Amber deserves its own word: it is the only colour that does not answer the question, it
reports that no answer exists. The capture keeps region tiles (the camera's region plus
neighbours within 64m) and void tiles (see below); a column outside all of them draws as a
short amber stub because a full line would claim a sky nobody has seen — and so does a
column inside a tile whose texel the capture never reached, which over the void is most
empty water. Amber is the honest colour; the alternative is confidently lying about ocean.

The Rain tab also carries the rain shadow and roof runoff overlay toggles, copied from the
Simulation floater — the same switches, not copies, since both drive the one render-debug
mask. The rain loop (trace, then the map the trace reads, then what the runoff sheds) needs
one window open, not two.

## Voidscape tiles

The capture used to end at the region borders. Beyond them every column fell to the
heightmap guess — which in the void means the water plane — and the column trace showed the
whole void as amber stubs. Region tiles were also the only tiles, so `resolveColumn` dropped
to its fallback the moment a column crossed a border, even one a neighbouring tile's
overscan had actually seen.

Two changes, both in `SSRainShadowMap`:

- **Overscan reads.** `resolveColumn` now tries the column's own tile first, then any other
  valid tile whose capture plane contains it — the ortho box reaches one overscan past its
  own footprint, so a column just across a border is mapped by whichever tile saw it.
- **Void tiles.** The super-grid squares past the rendered regions get their own captures,
  keyed by the square's region handle with a flag bit set. First ring captures at a quarter
  of `SSAtmoShadowRes`, halving per ring out to three rings — enough to cover the furthest
  tier radius the precipitation spawner can reach.

The void is **not** treated as open ocean. Objects, mega sculpties and (with the landscape
option) giant mesh landscapes sit up to a couple of kilometres past their parent region, so
a void square can hold anything from nothing to a whole mesh terrain. A miss over the void
is a miss exactly as it is inside a region: the landing falls back to the water plane, the
trace shows amber, and nothing claims an ocean nobody measured. What the tiles buy is that
geometry which *is* out there — dock edges, platforms, seafloor and landscape meshes inside
the capture band — gets mapped like any other geometry, at whatever square resolution it
lands in. `markDirty` walks every square an edit's radius touches, so a giant mesh rez goes
stale across its whole footprint, not just the square its root sits in.

Void tiles are deliberately second-class: they are the last thing `capture()` picks, never
run on a sky track (the track floor already answers every column), render into their own
small target so they never force the region target to reallocate, and rebake far less
eagerly — eight times the age limit, a 30s dirty interval instead of 2s, a looser
fall-direction drift epsilon, and a 2s minimum cadence on top. A far square mostly sees
still water or nothing between edits; genuine geometry changes out there arrive through the
dirty path within that interval. They never feed the surface field or drainage (those walk
region tiles), and the capture volume debug view draws them dimmed.

## Group resets

Every section heading on the four tabs carries a **Reset group** button beside it, alongside the
per-row `D` buttons that already reset one control each. The callback is `SSAtmo.ResetGroup`,
registered next to `SSAtmo.StrikeNow` in `llviewermenu.cpp` because these panels are plain XUI with
no class of their own to hang a callback on. The one exception is the Rain tab's Debug Overlays
heading: its checkboxes drive the render-debug mask, which is not a setting, so there is nothing a
reset could set it back to.

Its parameter is a **comma-separated list of control names, not a prefix**. A prefix would be
shorter and would need no maintenance, and that is exactly the problem: it would silently widen
the moment a setting was added whose name happened to start the same way, and the failure mode is
a button that destroys settings the person did not mean to touch. An explicit list can only ever
fall *behind* its panel, and an unknown name warns to the log and is skipped, so a stale list still
resets what it can. If a dial is added, add it to the list in the same file, a few lines above.

The Lightning tab needed splitting before it could have groups at all: forty-one controls under one
heading is not a group. It now divides where the dials genuinely do - **Lightning** (how often,
what kind, how the bolt looks in the air), **Ground Strike** (the 2026-09 rework's own family, the
one `doc/atmo_magic_lightning_strike.md` lists together), and **Advanced** (polarity, bolts from
the blue, occlusion culling, the bolt texture). The Clouds and LOD tabs both carry cloud settings
and their tooltips say so, since neither button touches the other tab's half.

## Related

- `doc/atmo_magic_env_ui.md` — the environment editor, which authors what these dials scale.
- `doc/atmo_magic_cloud_parallax.md` — the cloud field's rendering.
- `doc/viewer/ui_system.md` — floater registration and XUI mechanics.
