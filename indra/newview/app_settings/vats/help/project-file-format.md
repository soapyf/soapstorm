# Project file format

A VATs project (`.vat`) is a UTF-8 JSON file that holds everything about an animation: its curves,
timing, props, pins, simulations, audio and actors. The app and the
[[VATs Editor (viewer)|VATs Editor]] read and write the same format. This page is a reference for
people who read or generate project files with their own tools.

Project files and libraries saved by versions from before the rename, with their older extension and
`format` names, still open; saving them writes the current `format` names.

> Related articles: [[Projects and files]], [[Anim format]], [[BVH]]

## Format

A project is one JSON object. VATs writes the fields below; readers must accept them in any order.

```
{
  "format": "vats-project",
  "version": 1,
  "euler_order": "xyz-extrinsic",
  "fps": 30,
  "end_frame": 30,
  "loop": false,
  "loop_in": 0,
  "loop_out": 30,
  "priority": 3,
  "ease_in": 0.8,
  "ease_out": 0.8,
  "hand_pose": 1,
  "emote": "",
  "mirror_export": false,
  "export": {},
  "curves": {
    "mShoulderLeft": { "rot_z": [[0, 0, 2, 0, 0, 0, 0, 0, 0], [15, 40, 2, 0, 0, 0, 0, 0, 0]] }
  },
  "props": [],
  "anchors": [],
  "meta": {}
}
```

### Header

| Field | Value |
|---|---|
| `format` | `"vats-project"`. VATs also opens Hexton projects (`"hexton-sl-anim"`, `.hxanim`) |
| `version` | `1` for a project with one actor, `2` for a project with two or more actors (see actors below) |
| `euler_order` | `"xyz-extrinsic"`, the only order: a rotation `(x, y, z)` in degrees is X applied first, then Y, then Z |
| `meta` | a free-form object, always kept |

A file with a higher `version` than VATs knows opens with a warning and is saved only under a new name.

### Timing and export

| Field | Type | Default | Meaning |
|---|---|---|---|
| `fps` | int | `30` | frames per second, 1–120 |
| `end_frame` | int | `30` | last frame, 0–3600 |
| `loop` | bool | `false` | the animation loops |
| `loop_in`, `loop_out` | int | `0`, `end_frame` | loop points, in frames |
| `priority` | int | `3` | [[Animation priority]], 0–6 |
| `joint_priority` | object | absent | per-joint priority overrides, `{ "mHead": 4 }` |
| `ease_in`, `ease_out` | number | `0.8` | seconds |
| `hand_pose` | int | `1` | Second Life's hand pose, 0–13 |
| `emote` | string | `""` | the facial emote played with the animation |
| `mirror_export` | bool | `false` | the export is mirrored left to right |
| `export` | object | `{}` | export settings (below) |
| `ik_solve` | string | absent | `"literal"` in projects converted from Hexton: IK poses the way Hexton posed them |

The `export` object holds the settings of **File → Export**: `name`, `number`, `side`, `pattern`,
`folder`, `both`, `count_up`, the body `shape` (default `"sl-default"`), `reduce` as an array
`[degrees, metres]` of key-reduction tolerances, and `bvh_positions`.

### curves

`curves` is an object keyed by track, and each track is an object keyed by channel. Each channel is an
array of keys. Empty channels and tracks are not written.

| Track | Channels |
|---|---|
| a bone name, such as `mPelvis` | `rot_x`, `rot_y`, `rot_z` in degrees; `pos_x`, `pos_y`, `pos_z` in metres, as an offset from the rest position |
| an attachment point name, such as `Left Hand` | the same; the rotation is on top of the point's rest rotation |
| `ik.<Limb>`, such as `ik.ArmLeft` | `blend` (0–1; above 0 at any key means [[IK]] is used), `pos_*` the target position, `rot_*` the target rotation, `pole_*` the pole point |
| `pin:<joint>` | `rot_*` and `pos_*`, an extra offset on top of a pin's held transform |

Each key is an array of nine numbers:

| Index | Field | Values |
|---|---|---|
| 0 | frame | a number; fractions are allowed |
| 1 | value | degrees, metres or blend |
| 2 | interpolation of the segment after the key | `0` constant, `1` linear, `2` bezier |
| 3, 4 | left and right handle type | `0` auto-clamped, `1` auto, `2` vector, `3` aligned, `4` free, `5` flat, `6` plateau |
| 5–8 | left handle x, y, right handle x, y | absolute (frame, value) |

On load, VATs recomputes the handles of every key whose handle type is not aligned or free, so the
stored handles of those keys are ignored.

### props

An array of [[Props|props]], each with `path`, `name`, `bone`, `point`, `pos` (metres), `rot` (degrees),
`scale`, `visible`, `rigged` and `lib_id`. The app writes `path` relative to the project file when the
prop is on the same drive, and absolute otherwise.

### anchors

An array of pins (see [[Hold and bind]]):

| Field | Meaning |
|---|---|
| `joint` | the pinned joint or attachment point |
| `via` | the joint moved to hold it |
| `target` | the bone it rides; `""` holds it in the world |
| `from`, `to` | the first and last held frame; `to` of `-1` is open-ended |
| `pos`, `rot` | the held position (metres) and rotation (quaternion `x, y, z, w`) relative to the target |
| `start_key`, `release_key` | optional frames of helper keys VATs made |
| `target_actor` | optional: the actor whose bone it rides |

### dynamics

An array of [[Dynamics]] chains, each with `root`, `length`, `stiffness`, `damping`, `drag`, `gravity`,
`radius` and `baked`. A baked chain also has `source`: the chain's tracks from before the bake, in the
`curves` layout.

### ragdoll

The [[Ragdoll]], when set up: `whole_body`, `bones`, `start`, `frames`, `blend_in`, `blend_out`,
`gravity`, `stiffness`, `friction`, `baked`, and `source` when baked. `fall_direction` is one of
`"forward"`, `"back"`, `"left"`, `"right"`, `"random"` or `"none"`; absent means forward.

### audio

The [[Audio track]], when there is one: `path` (relative like props), `offset`, `volume`, `bpm`,
`beat_offset`, `beats` (an array of marked beats) and `snap`.

### Imported data

- `constraints`: the constraint blocks of an imported `.anim`, each a hex string of 86 bytes, written
  back unchanged on export.
- `orphans`: imported `.anim` joints that are not in the skeleton, each with `name`, `priority`, `rot`
  (`[seconds, x, y, z, w]` keys) and `pos` (`[seconds, x, y, z]` keys), written back unchanged on export.

### actors

A project with two or more actors (see [[Couples and groups]]) has version `2` and an `actors` array.
Each actor has `name`, `colour` (`[r, g, b]`), `body`, `pos` (metres), `rot_z`, `hidden` and `locked`.
The active actor's animation is the top level of the file, and `active` is its index. Every other actor
has a `clip` object with the same fields as the top level: timing, curves, props, anchors and the rest.
`fps`, `end_frame` and the loop settings are shared: the active actor's values apply to all.

### View settings

`onion` holds the [[Onion skin]] settings of the project.

### Unknown fields

VATs keeps every field it does not know, at the top level, in each actor and clip, and in pins, chains,
the ragdoll and the audio track, and writes them back on save. A tool can store its own data in a project
this way; a name with a prefix of its own avoids clashes with later VATs fields.

## Usage

### Saving safely

The app writes a project to a temporary file and renames it over the old one, so a failed save never
leaves half a file. The previous version is kept as `<name>.vat.bak`.

### Hexton projects

Opening a Hexton `.hxanim` project converts it (see [[Hexton import]]). `meta.migrated_from` records the
source file, unknown Hexton fields are kept under `meta.hexton_extra`, and the project is saved only
under a new name.

## Troubleshooting

### "not a VATs or Hexton project (format is missing or unknown)"

The file has no `format` field, or an unknown one. Check that it is a VATs project and not a pose or
prop library.

### "unsupported euler_order"

The file uses another rotation order. Convert the rotations to `xyz-extrinsic`.

### A prop or the audio file is missing after moving the project

Relative paths are relative to the project file. Move the prop and audio files with the project, keeping
their place relative to it.

## See also

- [[Projects and files]]
- [[Anim format]]
- [[Glossary]]

Category: Reference
