# Atmo Magic: roof runoff

> **OUT OF DATE** — archived 2026-08-26. Kept for historical reference; the code is the ground truth.

Rain that lands on a roof does not stop there. It runs down the pitch, collects
along the way, and comes off the eaves as a line of fat drips into the street —
which is most of what makes standing under an overhang in a downpour feel like
anything. The precipitation sim landed every drop correctly on the roof and then
forgot about it, so a sloped roof in heavy rain was completely dry underneath.

Runoff traces the surface the rain shadow map already captured into a drainage
network, works out how much water reaches each edge, and sheds it.

## Where the water comes from

Three sources, because none of them is enough alone.

**The analytic rate** is the backbone. `SSPrecipSim::dropRateAt` returns the
drops per square metre per second the individual-drop tier is asking for over a
spot — the same expression `spawnTierCell` builds its per-cell mean from, minus
the cell area and the tick rate. Multiply by an eave's catchment and you have
what accumulated impacts would average to: immediately, at any distance, with no
sampling noise. The gust envelope and the drifting area field are both in it.

Counting impacts *instead* would not work. The queue is radius-limited (32 m)
and capped (16384 entries), so a roof's runoff would depend on how close you
happened to be standing and would stop entirely for the roof across the street;
and a 4 m² patch gets a landing about once a second, so integrating that into a
believable flow rate needs a window long enough to lag the weather.

**The measured correction** is what impacts are genuinely good for. Every real
landing the queue dispatches is counted over a 4-second window and compared with
what the analytic rate says should have landed in the same disc:

    delivery = clamp(observed / expected, 0.35, 2.0)    // then smoothed

Every column lands on *something*, so this comes out near 1 in open country and
in a dense build alike — shelter does not bias it, because a drop landing on a
roof is still a drop that landed. What it does catch is the throttling and
capping between the weather parameters and the screen, which is real and which
the analytic rate cannot see. The result multiplies the shed rate.

It is deliberately a *correction on* a physically derived rate rather than a
replacement for it, and it is bounded hard: a bad window — the camera clipping
inside a wall, a region handover — must not be able to shut the eaves off or
open them into a flood. Drips schedule impacts of their own; those are tagged
`from_runoff` and excluded, or the system would read its own output as evidence
for more output.

**The reservoir** is what makes the drips steady. Each run holds the water
standing on the roof feeding it: catchment fills it, and it drains at
`store / 5s`. A roof does not stop draining the moment a gust lull passes
overhead, and a shed rate taken straight off the instantaneous weather made
every eave in sight stutter in time with the gusts. The store also carries
across a retrace, shared out by catchment, so a prim edit next door is invisible
rather than a pause.

## The drainage network

### 1. Resample the shadow map

`SSRainShadowMap::buildSurfaceGrid` reduces a captured tile (1024² depth texels
by default) to an `SSAtmoRunoffRes` grid — 256 across a region, 1 m cells.

The grid is **anchored to the region, not to the capture**. That is the whole
point: the tile is camera-relative — its vertical band and its footprint both
move with the camera — so a grid laid out in the tile's own space puts its cells
somewhere new on every recapture, and everything derived from it crawls around
as you move. Cell centres here are a pure function of the region, so retracing
lands on the same answer.

The fill is a **scatter, not a gather**: every texel is projected to where it
actually landed in the world and dropped into the region cell it fell in,
keeping the highest hit per cell — the first thing a falling drop would meet.
Gathering (one output cell reading a block of texels) is marginally cheaper and
wrong, because in an oblique projection a block of texels is not a column of
world space, and the mapping between the two shifts on every recapture.

Water is the one surface the depth pass does not draw, so a hit under the
waterline is the seabed and the cell belongs to the water above it. Cells the
capture missed entirely fall back to the heightmap; in a sky band they stay
empty, because that is open air.

One linear pass over the depth buffer; a full region is a couple of milliseconds.

### 2. Trace it downhill

Standard D8 flow: each cell finds the neighbour of eight with the steepest
descent. A cell is classified as:

- **Pooling** — nothing lower adjacent. Terminal, sheds nothing.
- **Water** — the cell belongs to the water plane. Terminal; that water is
  already where it was going.
- **An eave** — the step down is steeper than `SSAtmoRunoffEdge` (2.0 rise over
  run, about 63°) and at least 0.75 m. The surface *ends* here, so water
  arriving carries on into the air rather than onto the cell below.
- **Flowing** — everything else, including steep roofs. A roof is continuous
  however steep it is; a real eave is a *discontinuity in the capture*, where
  the step between adjacent samples is a whole storey rather than a course of
  tiles. That distinction is the whole trick, and `SSAtmoRunoffEdge` is the dial
  on it.

A cell can be both at once — part way up the rake of a gable the roof carries on
down toward the gutter *and* there is a sheer drop off the side — so an edge
also carries a **shed fraction**: how much of the water arriving there goes over
rather than past. Two things set it, and the larger wins.

The first is alignment: where the roof's own fall points the same way as the
drop, the two are the same edge and it sheds in full; at a right angle the water
is running along the edge and almost none of it leaves.

The second is whether the surface is still carrying the water anywhere at all.
Alignment alone cannot see a gutter, because along a gutter there is nothing to
align with: the lip is level to within the noise of the capture, so every cell on
it finds a neighbour a centimetre lower *along* the line, at right angles to the
drop. Read on alignment, a fifty metre eave is fifty cells of water flowing
sideways past the edge, and the only way off it is wherever the noise happens to
bottom out — which is what once put two drip lines on a roof that should have a
curtain of them. So the flatter the onward path, the more of the water simply
goes over the side: level to within about 2° sheds everything, and by about 10°
there is a roof pitch underneath it and alignment has the answer. Real pitches
start well above that, so a rake still keeps its water.

Wind is the third way onto an edge: rain driven across a gable end does run off
it, capped so a side edge in a gale cannot out-shed the gutter it drains into.

### 3. Accumulate

Cells are visited in **descending height order**. Water only ever moves to a
strictly lower cell, so this visits every cell after everything that drains into
it: no cycles to guard against, and one pass to do it. Each cell contributes its
own area and passes its total downstream, except at an eave, where the total
stops and becomes that eave's catchment.

### 4. Gather edges into runs

Water does not leave a roof at isolated points, it leaves along a line. A gable
lip is thirty edge cells in a row, and shedding those independently produces a
dotted line of drips with dry gaps between them — the one thing a roof edge
never looks like.

Edge cells are flood-filled into **runs**: cells join when they touch, sit at
much the same height, and shed the same way. A run carries the summed catchment
of its members, the mean outward direction, and the direction *along* the edge.
Drips then pick a member point and slide up to half a spacing along the run, so
they fall anywhere on the line rather than only at cell centres.

Runs are sorted by catchment and trimmed to 128 per region. A run longer than 64
points is thinned by stride rather than truncated, so drips still cover its
whole length.

### 5. Refine against the geometry

The trace runs on metre cells; the capture is centimetres. `refineEdge` walks
outward from each lip at the map's own full resolution and stops where the
surface does, so the drip leaves the actual edge of the object rather than the
middle of whichever cell happened to straddle it.

## When it retraces

Only when the region's **geometry** changes. The rain shadow tile behind it is
recaptured far more often than that — every time the camera climbs out of its
vertical band, and every time the wind swings the fall direction more than about
six degrees — and none of that changes the shape of a roof.

`SSRainShadowMap` therefore carries a **geometry serial**, bumped only by
`markDirty` (a prim update inside the band) and copied across at capture time.
The network keys off that serial, not the capture time. Standing still in
shifting wind traces once and then never again; a `traces` counter that climbs
while you stand still means something is churning prims.

## Shedding

Every frame, for runs within `SSAtmoRunoffRadius` (48 m):

    inflow  = catchment_m2 * dropRateAt(camera) * delivery * SSAtmoRunoffScale
    store  += inflow * dt                        (capped at 6s of inflow)
    outflow = store / 5s
    drips/s = min(outflow / SSAtmoRunoffMerge, 24)

`SSAtmoRunoffMerge` (12) is how many raindrops of catchment one visible drip
stands in for. Water genuinely does gather on the way down a slope and arrive at
the lip as fewer, fatter drips — and it is also what keeps a warehouse roof from
costing more particles than the rain falling on it. Each drip is scaled by the
**cube root** of the merge factor, so it reads as more water without turning
into a boulder.

Every run integrates its reservoir whether or not it is in range; only the drips
are gated on distance. Gating the accounting too would mean walking up to a roof
in a downpour and finding it dry for the first few seconds.

A drip is pushed to the effects pool rather than the drop tiers, so it never
competes with the rain for the tier population budget. It carries `PART_DRIP`,
which tells the renderer to give it the ordinary in/out fade instead of the
ripple pool's ramp-off — a ripple spends its life dying, a drip has to stay a
drop until it lands. It leaves the lip with a little downward speed (a streak is
oriented by its velocity, so a purely sideways start would draw the first frames
lying flat) and is ballistic from there.

The landing goes through the ordinary **impact queue**, so the street answers
with the same ripple, splash crown and ambient-loop contribution any other
landing gets. A busy eave therefore also *sounds* like one, for free.

### Streams

Past about 2 drips/s a run stops shedding drops at all. Water leaving an edge
that fast is a continuous fall, and drawing it as more and more separate drips
both reads wrong and costs a particle each — so the run puts up **streams**
instead, and the drips it still emits are cut right back to the spatter around
them — once the stream is carrying the water, a full share of drips on top of it
reads as rain coming off the roof twice.

A stream is a **curtain**, not a strand. The run is cut into slots and each
stream spans its own, hanging in the plane of the fall with the gutter running
through it, so together they are the length of the roof. How wide a slot should
be is not a number to invent: it is the width the art was drawn for, which the
preset already states as its sheet tier's quad — 18 m on rain, so a 36 m eave is
two streams and looks like the two quads of water it is. Cutting it to a
fixed couple of metres instead broke the same run into fifteen little ribbons,
each showing a sliver of a texture meant to span ten times as much.

They are not billboarded: a stream metres across has a real footprint in the
world, and
swivelling it to face the camera reads as a rack of strips turning as you walk
past. Nor are they hung level. Plenty of eaves are not — the rake up the side of
a gable, the two sides of a valley, a sloped awning — and a curtain hung
horizontally off one of those cuts straight through the roof it is running off,
half of it buried in the tiles and half hanging in the air above them. Each
stream carries the **pitch of the lip over the stretch it spans**, taken from
the two ends of its own slot, and its width axis follows that. The span is
widened by the same slope, because a slot is measured across the ground and a
sloped lip is longer than its own shadow: without that a rake ends up covered
by a curtain the length of the wall below it and the top of the run stays dry.
The lips are in order along the run and a run is already split where its
direction turns, so the slot's two ends are enough to say what its pitch is. Geometrically each is a ballistic ribbon of eight quads from the lip to
the landing — a straight quad cannot follow the curve — fading over the last
third so it meets the ground as spray rather than as a cut-off band.

The art comes from whichever tier's variants are closest in size to the water
being drawn. "Sheets or clusters" is the right instinct and neither answer holds
on its own: rain's sheet quads are curtains 18 m by 36 m, drawn for a shower seen
across a field, and its clusters are 0.76 by 3.8 — so a two metre fall off a
cottage eave takes clusters and a twenty metre one off a bridge deck takes
sheets. The pick is by ratio, so half the size counts the same as twice it, and
it is remembered on the particle: the texture is chosen once and the tiling is
worked out on every refresh, and if a widening stream could change the answer the
two would come apart. A configured drop texture goes through the same bake
rather than being laid on whole, or a stream is one drop the size of the whole
fall.

That choice is also what sets the tiling scale. The variants are baked with the
drops in them sized against *that tier's own quad*, so one repeat over one
quad's worth of world puts those drops at the size of the drops falling past the
stream. Repeats below one are the point rather than an accident to clamp away —
a stream is usually smaller than the quad its art was drawn for, so it shows
part of that art at true scale. Forcing at least a whole repeat squeezed the
entire texture into the stream instead and shrank every drop in it by the ratio
between them, which on rain's sheets is about ten. There is a per-stream offset
across, so the seams between neighbouring slots do not line up into a grid down
the gutter.

It scrolls down at the speed the water is running, which is what makes it read
as moving water rather than as a painted streak. The repeat count rides on the
particle and both sides use it: a feature sits at `f = (phase - v) / repeats`,
so the renderer laying out one count while the sim advanced the phase against
another had the water crawling on a short fall and racing on a long one.

`f` can be measured two ways and **Stream stretch** blends them, because they
are different pictures and both are worth having. The eight segments are cut at
equal times, and water in free fall covers about eight times as much ground in
the last of those as in the first. Measured by *distance* the art holds one size
the whole way down but scrolls at a steady speed. Measured by *time* it travels
with the water: a feature marks a parcel that left the lip at a given moment,
and since two parcels leaving a moment apart draw apart on the way down, the
column elongates toward the bottom — which is what real falling water does, and
is the default at 1.0. Shape follows distance either way, so the last third that
fades out is a third of the drop however the art is laid on.

What did come apart at the joins was the segment geometry. A segment was drawn
as a plain quad — one width and one colour for the whole of it — while a stream
varies continuously down its length, so both stepped eight times on the way
down. The width step was the visible one: it is there at every join regardless
of opacity, where a colour step only shows in the bottom third where the tail
fade is ramping. That is why setting the taper to exactly 1.0 hid the artefact
completely, which is what confirmed the diagnosis.

Colour is now carried **at each end** of a segment, evaluated at the two
boundaries so neighbours agree exactly where they meet. Width is not: a stream
does not taper at all. Making the ends unequal turns the segment into a
trapezoid, and a trapezoid textured as two triangles is interpolated affinely —
the halves disagree about where the art goes and every drop kinks along the
diagonal between them. A narrowing ribbon is not worth drawing the water in
zig-zags for, so streams are parallelograms of one width from lip to landing and
do their thinning with the fade.

### How far one falls

**Stream length** is metres of fall, not a fraction of the way down, because a
fraction means something different on every roof it is read on: the number that
looked right off a cottage eave drew a twenty metre column off the next tower
along. The ground caps it either way — a stream is never drawn past the point
its own drips land on — so a low eave still meets the street at any setting, and
only a tall one fades out in mid air, which is what a long drop does anyway as
it breaks into spray. The drawn length is turned back into a fall *time* and the
path walked from that, since the water is not falling at a constant speed and
both sides of the ribbon are laid out against time.

Wind is the other thing that ends a fall early. A stream leaves the lip going
outward and is then bent back by the wind over its length, and on a building
with any overhang that curve carries it back over the wall and draws a curtain
of water down the inside of the front room. Flattening the path out is the
obvious fix and the wrong one — water in a gale is exactly what the bend is
there for — so instead the fall is **cut short before it arrives**. Sample
points down the bent path are tested against the same surface the drips land
by: the shadow map's depth capture is what is over a column seen from above, so
a sample sitting below it is inside the building rather than in the air beside
it. The fall ends at the last sample still in open air, and the tail fade over
the last third turns the end into spray against the wall. Half a step of slack
either way is nothing against a fade that long, so eight samples is plenty and
the walk is skipped entirely when the water is going nowhere but down.

No two of them should read as the same curtain hung twice. Each takes its
**texture variant** and its **starting scroll phase** from its own key rather
than from the shared draw sequence — both are properties of the stream, and
hanging them off the order the random draws happen to come in ties them to code
above that has nothing to do with them. The phase matters more than it looks:
streams down one gutter are the same length and so run at the same speed, and a
row of them started at zero scrolls in lockstep. A whole repeat's worth of
offset covers every distinct starting point there is, since the phase only ever
enters the texture coordinate as an offset and the art tiles. There are eight
bakes and a gutter in a downpour puts up more streams than that, so half of them
also wear the art **mirrored across**, keyed off the same seed — free, and it
breaks up the pairs that do draw the same variant. Across only: flipping one top
to bottom would run the water up the roof.

Streams are **anchored**. Slot positions are a property of the run's length
alone, never of how hard it is shedding: a stream is refreshed in place while
the eave keeps asking for it and fades out over 1.2 s when it stops, so a gust
passing overhead makes the falls thicken and thin rather than slide along the
gutter. How hard it is running shows mostly in the opacity — with slots a whole
quad wide a run is one or two of them, and narrowing those to a thread would
leave the ends of the gutter dry rather than reading as less water. Widths vary
a few percent per slot, because streams cut to exactly the same width butt
together into one flat wall; only a few, because a tenth off an 18 m slot is a
metre of dry eave.

The thresholds are in drips per second, which is water divided by the merge
factor, and getting that wrong is what once left a whole town of running gutters
drawn as a couple of dozen streams. A roof face in heavy rain delivers a few
hundred raindrops a second to its gutter, which at a merge of 12 is a raw rate
in the twenties — but the old pair did not start a stream until six and did not
reach a full one until twenty, so every eave in a downpour sat at a fraction of
running. A run shedding a couple of drips a second is already running rather than
dripping.

512 streams may be alive at once across every eave in sight — hundreds is the
right number in a dense build, since every gutter on every roof around you is
running in a downpour. Runs are offered in descending catchment, so the budget
goes to the biggest gutters first, and the list is held in key order so finding
one is a binary search rather than a walk over all of them.

## Scope

Runoff only runs for **`LIQUID`** presets that make impacts. Snow settles on a
roof and embers rise off it; neither wants a drip line under the eaves.

## Cost

- Trace: a couple of milliseconds per region, and only when that region's
  geometry changes. At most one region per 0.5 s.
- Shedding: a walk over at most 128 runs per region.
- Drips are capped at 900 **live drips**, counted separately from the effects
  pool they share with ripples and splash crowns. Budgeting against the pool
  shut the eaves off exactly when it was raining hardest, which is backwards.

## What the preset controls

The settings below are the machinery — how much is traced, how far, how often.
What runoff *looks like* belongs to the weather, and lives in the preset editor's
**Runoff** tab, because a stream off a gutter is water seen from a few metres
away and nothing else in the sim draws this art that close.

| Preset field | Default | |
| --- | --- | --- |
| Stream opacity | 1.0 | A stream already fades up as its gutter runs harder; this scales that |
| Stream width | 0 | Metres of eave one stream spans; 0 takes the sheet tier's quad width |
| Stream length | 6 m | Metres of fall drawn. The ground caps it, so a low eave still meets the street; off a tower it fades out in mid air |
| Stream stretch | 1.0 | 1 the texture travels with the water and elongates; 0 it is pinned to the fall |
| Stream wind | 0.35 | How strongly the local wind bends the fall |
| Stream drop size | 1.0 | Size of the drops making up a stream, 1.0 being life size |
| Drip size | 1.0 | Size of the separate drips off an eave |

**Stream drop size** needs a word, because 1.0 is not what the texture actually
contains. The variant bake floors every splat at a minimum share of its quad, or
the far tiers come out as near-empty textures — a raindrop on an 18 by 36 m
sheet is a third of a texel, and a curtain of those across a field would be
nothing at all. That floor is right for what those quads are for and wrong at
arm's length: on rain it makes each drop **4.5× too wide**, which is exactly how
it looked on a stream. So a stream asks the variant builder what ratio the bake
applied, per axis, and tiles that much finer to undo it. The slider is the
preset's say on top of that.

## Settings

| Setting | Default | |
| --- | --- | --- |
| `SSAtmoRunoff` | on | Shed runoff at all |
| `SSAtmoRunoffScale` | 1.0 | Multiplier on how much is shed; 0 stops it |
| `SSAtmoRunoffMerge` | 12 | Raindrops of catchment per visible drip |
| `SSAtmoRunoffRadius` | 48 m | Runs within this of the camera shed |
| `SSAtmoRunoffRes` | 256 | Drainage samples across a region (1 m cells) |
| `SSAtmoRunoffEdge` | 2.0 | Slope that counts as an edge, not a steep roof |

All of them are on the **Simulation** floater. **Develop > Render Metadata >
Roof Runoff (Atmo Magic)** draws the network: blue arrows for flow, brightening
with catchment; red down the face each eave sheds by; and each run drawn as the
connected line it is, brightening with how much water it is holding.
