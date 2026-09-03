# Atmo Magic: per-track weather configuration

> **OUT OF DATE** — archived 2026-08-26. Kept for historical reference; the code is the ground truth.

Weather is configured per EEP sky track and only runs for the track the camera
is currently in. No track has weather until something defines it, so a region
with no Atmo configuration looks exactly as it does today.

## Tracks

`LLEnvironment::calculateSkyTrackForAltitude` numbers the sky tracks 1–4 using
the region's altitude settings:

| Track | Band | Ground zero |
|---|---|---|
| 1 | ground level, up to the first altitude | terrain / water surface |
| 2 | first to second altitude | the band's base altitude |
| 3 | second to third altitude | the band's base altitude |
| 4 | third altitude and above | the band's base altitude |

Track 1 behaves as before: precipitation lands on terrain, water and whatever
the rain shadow map captures.

Tracks 2–4 are sky bands. They get their own ground zero — by default the
band's base altitude — because the terrain thousands of metres below is not
what precipitation up there should be landing on. A column that finds no real
surface has no platform under it, so those drops fall through and fade instead
of pooling on an invisible plane. The `fallthrough` key controls how much of
that open-air precipitation is drawn at all.

## Where configuration comes from

The **baseline** is whatever was last loaded — a parcel notecard, a notecard
dropped onto the floater, or system defaults. Local edits sit on top of the
baseline as a **working set**, which is what the weather actually runs from.

This follows the environment floaters: edits away from the baseline show an
asterisk in the floater title, **Revert** discards them, **Defaults** clears
everything back to no-weather, and **Save As...** writes the working set out to
a new notecard.

### Parcel discovery

The server side cannot be extended to carry weather in the environment itself,
so the parcel description is used as the carrier. Put this anywhere in the
description of the parcel:

```
atmo:00000000-0000-0000-0000-000000000000
```

Replace the UUID with the asset ID of a notecard holding the configuration.
The marker can sit inside ordinary prose; the description stays readable.

Discovery is event driven, not polled — `LLParcelObserver` fires both when the
agent crosses into a different parcel and when the current parcel's properties
are re-sent, which is what editing the description produces. Each distinct
notecard is fetched once; re-entering the same parcel does not clobber edits.

Leaving a configured parcel drops back to system defaults. A notecard loaded by
hand from inventory is the user's own choice and survives parcel crossings.

### Loading by hand

Drag a notecard onto the Atmo Magic floater to load it. The **Load...** button
opens inventory filtered to notecards.

## Notecard format

LLSD, either XML or notation. **Save As...** writes LLSD XML.

```xml
<llsd>
 <map>
  <key>name</key><string>Storm front</string>
  <key>tracks</key>
  <map>
   <key>1</key>
   <map>
    <key>enabled</key><boolean>true</boolean>
    <key>preset</key><string>Rain</string>
    <key>precipitation</key><real>0.6</real>
    <key>turbulence</key><real>0.35</real>
    <key>wind_heading</key><real>225</real>
    <key>wind_speed</key><real>5.0</real>
   </map>
   <key>2</key>
   <map>
    <key>enabled</key><boolean>true</boolean>
    <key>preset</key><string>Snow</string>
    <key>precipitation</key><real>0.35</real>
    <key>wind_speed</key><real>2.0</real>
    <key>ground</key><real>1002</real>
    <key>fallthrough</key><real>0.25</real>
   </map>
  </map>
 </map>
</llsd>
```

The `tracks` wrapper is optional — a bare map of track numbers works too. Track
keys accept `"1"`, `"track1"` and `"track 1"`. A track present in the document
but with no `enabled` key still counts as enabled; the key exists so a track can
be switched off without deleting its settings.

### Keys

| Key | Type | Meaning |
|---|---|---|
| `enabled` | Boolean | Whether the track produces weather |
| `preset` | String | Preset name; omitted or unknown falls back to the editor's selection |
| `precipitation` | Real | 0–1, drizzle through to a full storm |
| `turbulence` | Real | 0–1, steady fall versus waves and bursts |
| `wind_heading` | Real | Degrees; 0 is north, 90 is east |
| `wind_elevation` | Real | Degrees off horizontal; positive blows upward |
| `wind_rot` | Array | Explicit quaternion, overrides heading/elevation |
| `wind_speed` | Real | m/s |
| `gust_depth` | Real | 0–3, how hard the wind surges and drops as a gust front passes; scaled by `turbulence` |
| `gust_length` | Real | Metres between gust fronts along the wind; divided by `wind_speed` this is how often a surge arrives |
| `gust_veer` | Real | Degrees the wind swings either side of its heading as a front goes through |
| `ground` | Real | Pins the track's floor to an explicit altitude |
| `fallthrough` | Real | 0–1, how much precipitation is drawn where nothing catches it |

### Wind direction

Direction is held internally as a **quaternion** rotating north (+Y) onto the
wind vector, so it carries elevation as well as heading — an updraught is just a
tilted wind — and composes with the rest of the viewer's orientation maths.
Strength is separate, in `wind_speed`.

It is *serialised* as `wind_heading` and `wind_elevation` because those two
angles describe a direction completely (roll about the wind axis is meaningless)
and are something a person can author by hand. `wind_rot` is honoured if
present, for tooling that would rather emit a quaternion directly.

Precipitation drift and the deterministic area field use the horizontal
projection of the wind; the audio mix uses the full magnitude.

## Behaviour notes

- **Crossing a track boundary crossfades.** A preset cannot be interpolated, so
  precipitation eases to nothing before a swap and back up afterwards. The same
  path handles a notecard arriving mid-session.
- **The master switch** (`SSAtmoEnabled`) still gates everything. Turning it on
  does not by itself produce weather anywhere.
- **Renaming a preset** in the preset editor follows the rename into every track
  config that referred to it, so nothing silently falls back to the default.
