# Atmo Magic: the environment editor's information architecture

The Atmo environment floater grew tab by tab as features landed, and the shape it ended up with
describes the renderer rather than the world. `Clouds` splits into `Volumetric Field` and `Sky
Dome` - two engine lineages, not two things a builder thinks about. `Atmosphere` contains a
sub-tab also called `Atmosphere`, and that page mixes scattering colour, optical phenomena and
scene tonemapping in one list. `Planetary` is a 122-line stub ranked equal to the 1038-line
Weather panel. This document records the re-cut around what an author is actually building, and the
reasoning behind each cut, so the next person to add a tab knows where it goes.

The assumed author arrives with a theme, not a parameter category: a realistic coastline, an
urban region, a geocentric fantasy world with two moons, a sky archipelago where the landmass
floats above a cloud sea and the ocean sits kilometres below, an alien planet, or a permanent
artillery barrage built by pointing the precipitation system at something that is not weather.
The architecture has to make all of those reachable without the author knowing which subsystem
draws what.

## Two rules for where a control lives

These are separate axes, and conflating them is what produced the current layout.

**Sub-floater versus panel is decided by workflow weight.** A celestial body editor needs a list,
per-body orbits, radii and inclinations, and room to work - that earns `floater_ss_atmo_planetary`.
A single dropdown does not. The environment floater's panels are for dial-and-see work; anything
with its own list-and-detail workflow gets a floater launched from the panel that owns it.

**Keyframe buttons are decided by animatability.** A param that varies across the day cycle gets
the prev/keyframe/next triplet and a `mFloatRows`-style binding. A param that is a property of the
track rather than of a moment does not. Most controls are panel and animatable; the planetary
scales are floater-launched with two animatable scales left behind in the panel; "weather falls
from" is panel and non-animatable. All three combinations are legitimate.

## Target structure

```
Track | Water | Clouds | Sky | Weather | Space | Look
  |              |        |       |
Template      Main Deck  Scattering   Conditions
dropdown      Under Deck Light & Glow Precipitation
              Dome                    Lightning
```

`Water, Land, Sky, Space` is the series the tab names sit in - hence `Space` over `Cosmos` or
`Astronomy`, the first overselling a two-slider panel and the second naming the study rather than
the thing.

`Look` holds the controls that are not the world at all: scene gamma, reflection probe ambiance
and horizon clipping are renderer output. Giving them a named home stops them accreting onto Sky
and gives future exposure or post dials somewhere to land. Look is also the only tab with no
position on the vertical axis, which is a decent confirmation it is carved correctly.

Templates live as a dropdown on Track rather than as their own tab. Seeding a world archetype sets
tracks, deck heights, water height and celestial bodies together - a sky world is not one setting,
it is water at 3km and a deck below the landmass and another above - but it is a track operation
and does not earn top-level rank.

### Renames and why

`Moisture Level`, `Droplet Radius` and `Ice Level` are not scattering dials. `skyF.glsl` multiplies
the rainbow map lookup by `moisture_level` and the 22-degree halo lookup by `ice_level`, and
droplet radius sets the arc geometry between them. They become **Rainbow**, **Rainbow Width** and
**Ice Halo** under a `Rainbow & Halo` header, which also reconnects them to the influence floater's
existing "Rainbows after rain" checkbox that has been driving a slider labelled "Moisture Level"
this whole time.

The UI prefix `atmo_moisture_level` becomes `atmo_rainbow` to match. The asset field stays
`mSkyMoistureLevel` and the LLSD key stays `moisture_level`: the serialised form is EEP's and must
not drift, so the UI name and the storage name diverge deliberately.

`Volumetric Field` and `Sky Dome` become `Main Deck`, `Under Deck` and `Dome` - what they are in
the sky, not which code path draws them.

The Weather Influence floater's haze row becomes **Rain thickens water fog** under a `Water`
header. It gated moisture -> haze (haze density up, distance multiplier down) as well as
precipitation -> water fog, and the haze half is retired: on skies authored with heavy haze the
+1.5 haze density lifted the fog term's airlight past what the tonemapper could hold and whole
scenes read as overexposed. The authored haze density and distance multiplier now render exactly
as keyframed, and the row is what it actually still gates. The asset fields follow
(`mHazeEnabled`/`mHazeStrength` become `mWaterFogEnabled`/`mWaterFogStrength`, LLSD keys
`water_fog_*` with the old `haze_*` keys still read); the mapping removal lives in
ssatmoenvskymodulator.cpp.

`Precipitation` is kept. It reads as forecast language, which is intended, and "stuff falling from
the sky onto people" covers the artillery case honestly enough that renaming it to a
mechanism-neutral word would cost more in tone than it gains in reach.

### Optics: halos split out of the texture strip

The old `ice_level` path was one histogram of the `halo_map` texture column stretched over the
whole 0-84 degrees around the light, which fused the sun's corona with a wide coloured ring into a
single texture read - not the 22-degree halo it claimed to be. Under an active Atmo environment
`skyF.glsl` now renders optics procedurally (`ssOptics`), each phenomenon at its true angular
position from the view ray around the light: the corona hugs the light, the 22° halo is a ring at
22°, the 46° halo family at 46°, sundogs where the 22° small circle crosses the light's altitude
plane, the circumzenithal arc arching over the zenith while the light rides below ~32°, and the
supralateral arc crowning the 46° ring when the light is low. Idle viewers (no active environment)
still get stock `halo_map` bit for bit.

Weather drives which phenomena speak and how hard - the Weather Influence floater gains an
`Optics` section with two rows. **Corona rings in misty air** is water drops: moisture, residual
droplets in the rain's wake (a few minutes) or light drizzle, freezes out below ~-4C. **Ice halos
from cold moist air** is the crystal family - the 22° veil of small platelets, the deep-cold 46°
family (large plates/columns lofter by a little convection), and still-air plate alignment for
sundogs and the circumzenithal arc. Every crystal drive gates on moisture as well as cold, so a
dry -30C sky renders no ice at all; the old Ice Level keyframed value is the base the weather
lifts from. The former `cold` row keeps only the dry-air blue-density shift ("Cold dry air
sharpens the sky colour"), which is why the old all-in-one "Cold clear air makes ice halos" is gone.

Two behaviours worth knowing when tuning. The optics ramp on the SUN'S horizon-band share
(`SSAtmoEnvApplier::sunRiseFraction`) - full while the disc's centre is up, easing out across the
twilight below the horizon - not on the stock centre-crossing test - a low sun's halos burn in
from the first sliver of quad above the horizon and fade through the dusk after it sets instead
of popping at centre-rise, and the ring keeps aiming at the sun's true position the whole way.
And the crystal "frost" drive ramps from nothing at freezing down to full at -20C; there is
deliberately no ice anywhere above 0C, so scrubbing temperature fades halos in and out across the
freezing band rather than snapping them off.

## The altitude rail

The floater already carries a vertical multislider on the left whose thumbs are the track selector.
It gains a second mode instead of a second widget, because the rail is already doing two jobs
(altitude and selection) and adding a third kind of object to the same markers is the exact
conflation this rework removes.

| | Track mode (Track tab) | Layer mode (every other tab) |
| --- | --- | --- |
| Scale | fixed 0-4096, increment 64 | fitted to the selected track's contents |
| Markers | every track, toggling, selecting | water, main deck, under deck, weather bracket |
| Anchors | none | Space at the top, Dome directly below, both out of scale |
| Buttons | Add Track / Remove Track | Add Deck / Remove Deck |
| Click | selects that track | selects that layer and switches to its tab |

```
  +---+
  | * |  Space                    fixed anchor, selects Space
  +---+
  | ~ |  Dome        6000 m       fixed anchor, height as text, selects Clouds > Dome
  |===|  --------------------     fit range top
  | # |  Main Deck    800 m       selects Clouds > Main Deck
  | : |   weather                 bracket, selects Weather
  | = |  Water         20 m       selects Water
  |   |  -- track floor 0 m --    baseline tick
  | # |  Under Deck  -200 m       only when enabled
  +---+  --------------------     fit range bottom
   [+ Add Deck]  [- Remove Deck]
```

A fixed scale cannot serve both a coastal build with everything inside 100m and a sky archipelago
spanning 10km, and a piecewise scale is worse: a rail where the same mouse travel means 64m at one
end and 800m at the other is unpredictable to drag, breaks the increment model, and makes
`overlap_threshold` enforce a different pixel gap per segment. Fitting to content keeps the scale
linear everywhere and the precision uniform, and the mode switch means the author never has to ask
for a zoom level - Track mode shows the region, every other tab shows the track you are inside.

The transition interpolates over roughly 200ms on both the scale and the marker cross-fade, so it
reads as diving into the selected track rather than as the widget swapping its contents. Re-fitting
happens on drag commit, never during a drag, so the axis cannot move under the cursor.

The fit reads decks through the same auto derivation the renderer resolves, so a deck with `Auto`
ticked tracks its height rather than the stale authored row it greys out. The fit's own bottom and
top round to the nearest 256m: an auto deck's height wanders with moisture and convection, and an
exact fit would glide the whole scale under every step of that drift, where a quantised one only
moves once the content crosses a block boundary.

Space and Dome are fixed anchors excluded from the fit. The dome is a backdrop rather than a placed
layer, its height is frequently on `mAuto`, and a cirrus dome at 6km would otherwise squash the
decks being edited into the bottom eighth of the rail.

Sky and Look have no marker. Sky is the medium filling the band rather than an object in it; Look
is viewer-facing output. Both leave the rail in layer mode holding context with nothing highlighted.

### The weather bracket

Precipitation occupies a span, so it draws as a bracket rather than a thumb. The layer rail and
everything on it live in the track's own frame: the water plane's height and both decks' base
heights are metres relative to the track floor (`SSAtmoEnvTrack::mFloorZ`), negative below it, and
the renderer's resolvers add the floor back where they render (`visibleWaterHeight()`, the cloud
field resolver). The reference surface is therefore `max(0, water height)` in the rail's frame,
with the track's own `mWater.mEnabled` deciding whether the water term counts at all. The top is
the delivering deck.

The delivering deck defaults to the lowest enabled deck above the reference surface, which resolves
correctly for the sky case by construction - the under deck hangs below the platform floor, so it
is under the surface and the main deck delivers. A **Weather falls from** dropdown at the top of
Weather > Precipitation lets an author override it, for the case of wanting weather from the upper
deck while a lower one is enabled for looks. It is not animatable. If the selected deck is disabled
or removed it reverts to the main deck rather than leaving weather with no source.

## Falling is its own switch

Moisture was the sole answer to "is it raining", and it is also the answer to "how much cloud" and
"how hard does it fall". An overcast, gloomy, thundering sky with nothing coming out of it - the
commonest sky there is - could not be authored at all: any moisture high enough to be overcast
rained.

**Weather > Conditions > Falling** is a keyframed bool (`SSAtmoEnvWeather::mPrecipitationFalls`,
LLSD `precipitation_falls`, default true, written only when authored so old documents keep raining
and new ones stay clean). Off folds into the resolver's existing clear branch
(`SSAtmoEnvWeatherResolver::resolve`) after the okta cover has been banked: type, intensity, droplet
size and impact scale all read clear, and *nothing else* changes - cloud cover, deck gloom, storm
darkening, gusts and lightning never consult it. Every downstream consumer already keys off the
resolved type and intensity (the bridge's `mPrecipitation`, the applier's rain-stopped clock, wetness,
the surface field, soundscape), so suppression propagates without a single new call site.

Keyframed rather than a plain track flag because *when it starts and stops* is the whole ask. Flag
keyframes HOLD (`ss_atmoenv_default_curve<bool>`): a bool has nothing to interpolate, so the value
stands from its own key until the next and the shower runs from the key that ticks the box to the key
that unticks it. Bool rows draw scrubber ghosts like every other keyframed type, so a day cycle's
showers are read straight off the rail: an `on` mark where it starts and an `off` mark where it
stops.

Those marks sit at the keys themselves and fill only when the head is on one. A HOLD ghost briefly
drew mid-span and filled anywhere across the stretch its value covers, which put a keyframe mark at
a phase the head could never land on to edit, and made a three-hour shower look like three hours of
keyframes. Filled has to mean here what it means on the row's own keyframe button - *the head is on
this key* - or the two disagree about the same key at the same instant.

They used to be EASE, which on a bool is not a curve at all - it is a step at the segment *midpoint*,
so the rain started halfway between the key that turned it on and whatever key happened to precede
it, a boundary in a place the author never put a mark. EASE was never authored intent on a flag, it
was the generic default landing on a type with no use for one, so `ss_atmoenv_curve_on_load<bool>`
corrects it on the way in rather than migrating documents.

The forecast line follows the resolved state instead of raw moisture, or a suppressed SEVERE sky
would still announce "Thundery showers" over a dry street; it reads "Overcast and strong winds"
instead. The type row's proof text gains a third answer, `Off`, reported even under a forced type,
because "the author switched it off" and "the air is dry" are different states that both used to
read `Clear`.

A "None" entry on the type combo was rejected in its place: `presetNameForType()` passes unknown
names through and falls back to the *active* preset, so a build that did not know the name would
render rain anyway - where an unknown LLSD key is simply ignored and behaves as today.

## The top chrome: forecast strip over the scrub

The band between the Name field and the preview scrubber holds a forecast strip - one column per
couple of hours across the whole cycle, laid out the way BBC Weather lays out an hourly forecast:
a condition glyph at the head, then temperature, then how much falls, then a wind rose with the
speed inside the ring and a barb on the bearing the air travels. Columns take their x from the
scrubber's own rect, so they stand over the part of the timeline they describe and stay in step with
the head.

One row is deliberately *not* on the column grid. The precipitation marks are a continuous band
sampled every ten pixels across the whole rail, kept only where something is actually falling, so
they start and stop where the rain does. Columns are hourly readings, and a shower is not hourly: a
spell running 07:40 to 09:20 falls between two-hourly columns and reads as a whole morning of rain
from them, where the band draws its actual extent.

Every mark stands on one baseline and grows upward, so the eye compares the tops - where the
difference is - rather than hunting for it around a centre that shifts with each glyph's size. That
alignment is also what makes the heads legible: a disc and a bar only tell apart when they start
from the same place.

Weight is a seven-step ladder, one rung per intensity band, laid out as a table in enum order so the
table *is* the ladder and a new band would be a new line:

| band | mark | head |
| --- | --- | --- |
| Drizzle (light) | one dot | - |
| Drizzle | two dots | - |
| Drizzle (heavy) | two dashed streaks | - |
| Light | two streaks | - |
| Moderate | two streaks | small puddle |
| Heavy | three streaks | large puddle |
| Torrential | three streaks, longest | wide flat slab |

The heads are half-discs sitting on the baseline rather than whole ones floating over it - what has
collected on the ground is what they stand in for, and a puddle has a flat bottom. That also gives
the slab something to differ from: a full disc against a squat rectangle is close to the same
silhouette at four pixels, where a round dome against a wide flat sheet is not, so the slab widens
and flattens to lean into the difference.

Every adjacent pair differs by at least one whole feature, which is the property that matters: a
mark is read against its *neighbours* in the band, not against a legend. Dots become dashes, dashes
become solid lines, a puddle appears at the foot, a third streak arrives over a bigger puddle, and
the puddle finally spreads into a slab. Non-adjacent pairs differ by more than one, so the ladder
degrades gracefully - a mark misread by one step is still nearly right.

The drizzle family carrying no head at all is the clearest division in the set, and it lands where
the resolver puts its own: `classifyIntensity` only hands out the drizzle bands for liquid types
(`isDrizzleCapable`), so a headless mark means water light enough to drift. Snow and hail scale
smoothly with the band's position instead of changing form - a flake is a size, not a count, and a
pellet grows and then fills once it is the size that dents cars.

One thing deliberately does *not* vary across the ladder: the lean. Every streak stands at about 29
degrees off vertical, held there by expressing the slant as a fraction of the streak's own length
(`STRIP_RAIN_LEAN`) rather than as a fixed pixel offset. A flat two pixels was 27 degrees on a short
drizzle streak but 16 on a long heavy one, so the harder it rained the more upright the rain stood -
and an angle that changes with intensity reads as wind rather than as weight.

One thing was tried and dropped: a three-lane vertical stagger, on the theory that a row of
identical marks would read as a dotted rule. What it actually produced was a repeating sawtooth, and
once the marks carried intensity in their own shape it was noise the eye had to subtract before it
could compare anything.

The preview head draws as a vertical line from the top of the strip down into the scrubber's thumb.
Stopping it at the strip's floor left the strip and the control reading as two stacked things with a
coincidence between them; carried into the thumb they are one instrument, and every row it crosses
is read against the head by following the line.

The one departure from that layout is the hour, which sits at the foot rather than heading the
column. A printed forecast sets the time on top because the column is the whole story; here it is
not - the scrubber below it is, and putting the label against the timeline it indexes lets the two
be read as a single scale.

Every mark is drawn from the viewer's own 2D primitives (`gl_circle_2d`, `gl_line_2d`,
`gl_rect_2d`, `gl_triangle_2d`) - no textures and no font glyphs. A cloud is three discs over a
flat rect, the sun a disc with eight rays, rain a leaning line with a dot at its foot, snow three
crossed lines, hail a ring, the bolt two thin triangles. That keeps the set extensible without an
art round-trip and keeps it legible at the 20-odd pixels a column can spare, which a downscaled
texture would not be.

It replaces a one-line prose forecast that used to sit in the same chrome. That line said one thing
about one instant, which is the least useful thing to say about a *keyframed* weather cube: what an
author is authoring is the shape of a day, and the shape is exactly what a sentence cannot show. The
sentence itself is kept, moved down onto `Weather > Conditions` at the head of the very rows it
summarises - it is only ever read while those numbers are in front of you, and it is redundant with
the strip anywhere else.

The strip is drawn in `SSFloaterAtmoEnv::drawForecastStrip()` rather than built as widgets. At
thirteen columns of five marks each it would be some seventy display-only controls, none of which
ever takes a click and every one of which would need repositioning by hand on each resize. The
vertical rack lives in the `STRIP_*` constants; the floater XML reserves `STRIP_HEIGHT + STRIP_GAP`
between the name row and the scrubber and nothing else, so moving a row means moving the scrubber's
`top` with it.

Two details are load-bearing. `STRIP_GAP` clears the lane `drawKeyframeGhosts()` writes value labels
into when a row is hovered, which is why the wind row does not sit flush on the scrubber. And
daylight comes from the track's own rise/set (`SSAtmoEnvPlanetaryResolver::riseSetPhases`, the same
call the scrubber's sun and moon markers use), not from a hardcoded six-to-six day - a tilted or
high-latitude world is the kind of thing this editor exists to author, and a strip that drew a noon
sun through a polar winter would be lying about it.

The step coarsens - hourly, two-hourly, three, four, six - until a column has room for its widest
line, so a narrow floater thins the strip out instead of smearing it. It never goes finer than
hourly: past that the columns are reading interpolation noise rather than weather. The band below
them is unaffected - it is sampled off the pixel pitch, not the hour, because what it draws is an
extent rather than a reading.

### Anchoring, while we were here

The floater was widened at some point and the body was not re-fitted: the tab container kept a
586px width that left roughly 200px of empty floater beside it, and the scrubber kept a fixed 530.
Both are now anchored to the floater's right edge. Inside the panels the spinner-and-keyframe column
is anchored right and the sliders carry a right anchor instead of a width, so the slack goes into
the sliders rather than into a dead band - which is the same "one right-aligned spinner column"
rule the panels were already built around, just made to survive a resize.

The stretched sliders carry an explicit `left` and `right` rather than keeping their `left_pad`
and gaining a `right`. `ParamValue<LLRect>::updateValueFromBlock` only prefers the two edges when
*both* are provided, and `LLView::applyXUILayout` clears `rect.left` whenever a `left_delta` (which
is what `left_pad` becomes) is in play - so `left_pad` plus `right` falls through to a branch that
happens to work but is not the documented pairing. Every rewritten row resolves to the exact rect
it had before at the design width; only the follows flags decide what happens after that.

## Rolling a day: Randomize and Remove

`Weather > Conditions` carries two buttons beside **Weather Influence...**: **Randomize**, which
rolls a whole day of weather into the selected track's cube, and **Remove**, which clears it back
to a still, dry, clear sky. Both act on one track - the cube is per-track and so is everything else
on this floater.

The generator (`ssatmoenvweathergen.cpp`) writes *curves*, not constants, and only the five the cube
actually holds: moisture, convection, temperature, wind heading and wind speed, plus the
precipitation switch. It never touches a cloud deck, a sky colour or a lightning dial. That is the
whole trick, and it falls out of the resolver's existing shape: cover in oktas, intensity band,
precipitation type, storm darkening, gust behaviour and lightning cadence are all *derived* from
those five, so a generator that gets the curves right gets a day right and never has to know what a
cloud deck is. It also means every extreme event is a bend in a curve rather than a mode - "blizzard"
is nowhere in the code as a state, it is cold air plus a wet deck plus hard wind, and
`derivePrecipitationType()` reaches the word by itself.

### Spells, and why precipitation has four stages

The unit the generator works in is a *spell*: a lead, a fall, an ease and a clear. Real weather
arrives, and a single "moisture goes up here" keyframe collapses that into a sky that snaps from
blue to raining. The lead is the load-bearing part - it puts an overcast deck over the region
*before* the first drop, because moisture is cloud cover as well as rain intensity, so raising it
ahead of the switch is literally the sky darkening in advance.

The switch itself is two keys per spell and nothing between them - on at the start, off at the end -
because flag keyframes HOLD. Not even a key at phase 0: the wrap segment holds the *last* key's value
backwards through midnight, which is the off the last spell ended on.

Spells are confined to phase 0.05-0.90 rather than allowed to wrap midnight. Wrapping any one of the
four stages past phase 1 turns simple arithmetic into modular arithmetic in five places; the cost is
that no *rolled* storm runs through midnight, and an author who wants one drags it there in seconds.

### Themes and events

Four times in five the roll is seasonal - spring, summer, autumn or winter, each a band of
temperature, baseline moisture, convection, wind and how many spells it tends to produce. The bands
are read off what the resolver does with the numbers rather than off a climate table: autumn's 0.30
baseline moisture is "four oktas standing over you all day", and winter's temperature band is chosen
so that `derivePrecipitationType()` stops returning rain, which makes a winter spell a snow spell
without the generator saying the word.

Roughly half of seasonal rolls also draw an extreme event from a season-filtered pool -
thunderstorms, a squall line, a hailstorm, a blizzard, a cold snap, a heatwave, a gale, or a
fog-bound morning. Events bend the bands *before* the curves are laid, so they never rewrite
keyframes the season already wrote. A cold snap in particular is a bodily drop rather than a re-roll,
so an autumn day of showers survives it as the same day of sleet.

The remaining one in five is a fantasy archetype - the Stormlands, an Ashen Sky, an Endless Winter, a
Glass Calm, a Weeping Season, the Tideturn, an Emberfall - each a whole world rather than a day, and
each deliberately outside the envelope the seasonal path stays inside. No event is layered on top of
one: sanding an archetype's edges off removes the only reason it exists. They still run through the
same spell machinery, so an impossible sky still arrives and clears like a real one.

Every key the generator writes snaps to the preview scrubber's grid through `ss_atmoenv_snap_phase`,
the same helper the sky seeding uses. The head moves in those steps and nowhere else, and
`hasKeyframeAt` matches within a tenth of one, so an unsnapped key at 0.3174 is present in the curve
but visitable by nothing: the diamond never lights, prev/next lands beside it, and removing it means
reaching a mark the scrubber cannot stand on.

A line under the rows says what the roll turned out to be. A generator meant to be pressed
repeatedly has to report itself, or the button is a slot machine with the reels hidden - eight
changed slider positions do not tell you whether to roll again.

### The two confirmations, and why only one

Randomize is unconfirmed on purpose: it exists to be pressed until a day looks right, and a dialog
between presses makes that loop unusable. Remove asks, because it reads as deleting work and has no
second press to soften it. Both are undone wholesale by Revert, like every other edit here.

### Reading a roll off the strip

The forecast strip's precipitation figure is taken from **moisture**, not from the resolved
intensity - the same number except that suppression zeroes it. Grey means nothing reaches the ground
that hour, whether because the air is dry or because the author switched precipitation off; the
figure still says how wet the sky is. So a grey 0% is a clear day and a grey 60% is a deck holding
its water, and an authored lead-in reads directly off the strip: the figure climbs greyed while the
deck thickens, then turns colour at the first drop.

## Precipitation types: two tiers

There is one preset tier where there need to be two.

| Tier | Edited by | Stored in | Editor |
| --- | --- | --- | --- |
| Shipped types | us | viewer install | `floater_ss_atmo_preset`, kept as the dev tool it is |
| Environment types | authors | the Atmo environment asset | new floater launched from Weather |

`floater_ss_atmo_preset` already holds archetype (liquid, flake, solid, riser), drop shape,
emissive, shatter, the three render tiers, landing rings, splash crowns, texture lists and sound
packs. That is the most theme-defining editor in the system and it is currently reached through the
viewer settings floater, two floaters away from the world it describes. Authors get their own tier
reached from Weather > Precipitation: the type combo lists shipped types and this environment's
own, visually distinguished, with **New from this...** deriving a local type and **Edit...**
opening it.

A derived type carries a full copy rather than a reference to its parent, so that a viewer update
retuning stock rain cannot silently change a shipped region. For the same reason, shipped type
definitions are copied into the asset on save as well: every environment is then self-contained and
a keyframe referencing a type that a given build does not have becomes impossible rather than
needing a fallback policy.

Camera shock and impact sound land in that editor as disabled stubs, revealed when the archetype is
impact-capable, so the UI documents the intent before the feature exists.

## What the rework touched

**The panels.** `panel_ss_atmo_env_atmosphere*.xml` became `panel_ss_atmo_env_sky.xml` with
`_sky_scattering` and `_sky_light` beneath it; `panel_ss_atmo_env_planetary.xml` became
`_space.xml` and took star brightness with it; `_clouds_volumetric.xml` became `_clouds_main.xml`;
`panel_ss_atmo_env_look.xml` is new; and the single Weather panel split into `_weather_conditions`,
`_weather_precipitation` and `_weather_lightning` behind a thin container.

Relocating a control between panel files needed no C++ at all - the floater reaches everything
through recursive name-based `getChild`, so a name that stays unique and still exists somewhere in
the tree keeps working. The only code change the whole regrouping pass required was one row prefix,
`atmo_moisture_level` to `atmo_rainbow`.

**The rail.** `railCentreForValue()` now maps through `mRailMin`/`mRailMax` rather than reading the
slider's own range, which is what lets the scale animate. `refreshRailMode()` reads the selected
tab and does the widget swap; `refreshLayerRail()` populates the layer markers; `draw()`
smoothsteps the range across the mode change. Thumbs are only added once the zoom settles, because
the slider clamps values to its own range and would otherwise snap a deck sitting outside the
interpolated window onto its edge and write that back. `LLMultiSlider` gained a
`setOverlapThreshold()` setter: the authored 304m gap is sized for the 0-4096m scale and rejects
every marker on a fitted one.

**Precipitation tiers.** `SSAtmoEnvAsset::mPrecipitationTypes` holds the environment's own types as
serialised `SSPrecipPreset` documents, staged into the live preset list by
`ssAtmoEnvStagePrecipTypes()` when an environment is adopted and dropped on unload.
`ssAtmoEnvEmbedReferencedPrecipTypes()` runs at save. `SSAtmoEnvBridge::presetNameForType()` now
passes unrecognised names straight through instead of returning an empty string, which is what lets
an author-named type resolve at all; a name that resolves to nothing still falls back to the active
preset exactly as before.

The editor is `SSFloaterPreset` in a second scope rather than a second floater. Opened with a map
key carrying `scope: environment`, the same widgets read and write the asset instead of disk, and
the viewer's own running precipitation is left alone. One editor rather than two so the tiers
cannot drift apart field by field; a bare string key is still the viewer-scope call it always was.

## Still to do

Camera shock and impact radius exist as disabled stubs on the preset editor's Impact tab. Nothing
reads them yet.

The world template values in `ssAtmoEnvTemplates()` are starting points rather than authored
presets. They place a coherent stack for each archetype - the sky archipelago puts water 2000m
below the track floor with an under deck 900m above it and the main deck 2600m up, the barrage
puts a thick dark deck 700m up (all heights floor-relative) - but the colours and weather numbers
want dialling against the real renderer.

## Deliberately not done

Accordions were considered and rejected: the panels are navigated often enough that managing
open/closed state is a cost, and the wide keyframe-button rows do not survive the squeeze. Grouping
is done with in-panel headers, which the panels already use.

A basic/advanced disclosure was considered and rejected: hiding EEP-parity dials treats the author
as a novice and papers over grouping problems rather than fixing them.

Generalising `mCloudField` and `mUnderField` into a vector of decks is deferred. Two named decks
with distinct semantics - the main deck always on and emptying via coverage, the under deck opt-in
and seeded off - is what the sky-build case needs, and the duplicated `cloud_*` / `ucloud_*` control
sets are already built and working. The rail's Add Deck and Remove Deck read as generic but bottom
out on the under deck's enable flag. If a third deck is ever wanted, the rail does not change: only
the asset gains a vector, `fromLLSD` gains a compatibility path for the old `cloud_field` and
`under_field` keys, and the deck panels collapse into one rebound to a selection.
