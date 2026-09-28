# Anim format

An `.anim` file is Second Life's binary keyframe animation, the format the viewer plays and the upload
server stores. VATs reads and writes version 1.0 and reads the older version 0.1. This page describes
the layout and how VATs fills it; for the export steps, see [[Export to Second Life]].

> Related articles: [[Export to Second Life]], [[Animation priority]], [[BVH]], [[Skeleton]]

## Format

All numbers are little-endian. `U16`, `S32`, `U32` and `F32` are 16-bit unsigned, 32-bit signed,
32-bit unsigned and 32-bit float. A `cstr` is raw bytes up to and including a NUL.

The file has no magic number. It starts with the bytes `01 00 00 00` (version 1, sub-version 0).

### Header

| Type | Field | Values |
|---|---|---|
| U16 | version | `1` |
| U16 | sub_version | `0` |
| S32 | base_priority | VATs writes 0–6; see [[Animation priority]] |
| F32 | duration | seconds, at most `60.0` |
| cstr | emote_name | empty, or an `express_*` name |
| F32 | loop_in_point | seconds |
| F32 | loop_out_point | seconds |
| S32 | loop | 0 or 1 |
| F32 | ease_in_duration | seconds |
| F32 | ease_out_duration | seconds |
| U32 | hand_pose | 0–13 |
| U32 | num_joints | 1–216 |

With an empty emote, the header is 41 bytes. With **Loop** off, VATs writes loop in `0` and loop out
equal to the duration.

### Joint records

`num_joints` records follow the header:

| Type | Field | Values |
|---|---|---|
| cstr | joint_name | a skeleton bone or attachment point, never `mRoot` or `mScreen` |
| S32 | joint_priority | `-1` (use base_priority) or 0–6 |
| S32 | num_rot_keys | ≥ 0 |
| 8 bytes each | rotation keys | U16 time, U16 x, U16 y, U16 z |
| S32 | num_pos_keys | ≥ 0 |
| 8 bytes each | position keys | U16 time, U16 x, U16 y, U16 z |

- **Time** maps `0…65535` over `0…duration`, a resolution of 0.92 ms at 60 s.
- **Rotation** is the vector part of a unit quaternion with `w ≥ 0`, each part mapped over `-1…1`. It is
  the joint's local rotation relative to its parent and replaces the rest rotation.
- **Position** is in metres, each part mapped over `-5…5`. For `mPelvis` it is an offset from rest; for
  every other joint it is the absolute local position.
- The viewer interpolates linearly between keys: nlerp for rotations and lerp for positions.

### Constraints

An S32 count (0–10) follows the joints, then 86 bytes per constraint: chain length, type (point or
plane), a source collision volume and offset, a target volume (or `GROUND`) and offset, a target
direction, and four ease times. The viewer ignores the whole section when the count is outside 0–10.

### Size

```
41 + len(emote) + Σ over joints (len(name) + 1 + 12 + 8 × (rot keys + pos keys)) + 4 + 86 × constraints
```

The 4 is the constraint count, written even when it is 0. The Second Life upload server refuses files of
250,000 bytes or more.

### Worked example: the size of a file

[Open the example](example:graph-basics.vat), the arm wave from the [[Graph editor]] page, and export it
with the default settings ([[Export to Second Life#Worked example]]). The status bar reports 1337 bytes,
which the formula reproduces:

| Part | Bytes |
|---|---|
| Header, no emote | 41 |
| `mCollarRight`: 13 + 12 + 8 × 16 rotation keys | 153 |
| `mShoulderRight`: 15 + 12 + 8 × 32 | 283 |
| `mElbowRight`: 12 + 12 + 8 × 73 | 608 |
| `mWristRight`: 12 + 12 + 8 × 28 | 248 |
| Constraint count, 0 constraints | 4 |
| Total | 1337 |

No bone has position keys: none of them moves from its rest position. The elbow keeps a key on all 73
frames because it never stops turning; the collar, which only rises and comes down, keeps 16.

## What VATs writes

- Records in skeleton order, then attachment points, using canonical bone names.
- Only bones and points that have keys, or that IK, pins or dynamics move. A bone keyed on one channel,
  or on position only, still gets a full record with rotation keys.
- A key at every whole frame from 0 to the last frame, baked against the export **Bake shape**, then
  reduced as described in [[Export to Second Life#Reduce keys]]. Reduction keeps the first and last
  frames and the frames where you set keys.
- Rotations in parent space, including the bone's rest rotation. The quaternion is normalised and
  negated when `w < 0`.
- Position keys only for bones whose position moves: a bone other than the hip whose position stays
  within the position tolerance is written without them (see
  [[Export to Second Life#Positions that do not move]]). A non-hip position is the bone's default
  position plus the move, or with **Your avatar** the position on the avatar you wear plus the move.
- Positions further than 5 m are clamped, with a warning.
- Values quantised exactly as the viewer does it: floor rounding, encoded twice, in 32-bit float
  arithmetic, so the values the viewer decodes match VATs' preview.
- Per-bone priorities as `joint_priority`, and `-1` for bones that follow the clip.
- Imported constraints and imported joints that are not in the skeleton, unchanged.

## Import

**File → Import SL .anim...** reads version 1.0 and the legacy version 0.1 (Euler-degree keys in 32-bit
floats). The import replaces the current clip, marks the project untitled and modified, and keeps its
props.

- **Frame rate.** An `.anim` stores times, not frames. VATs tries 30, 24, 25, 60, 50, 48, 15, 12 and 10
  fps, then 1 to 120, and picks the first rate at which every key lands on a frame. If none fits, it
  uses 30 fps and keeps the keys on fractional frames. To use another rate, change **Properties →
  Animation → Frame rate** and pick **Keep Timing**.
- **Values.** VATs keeps each key at the middle of its quantisation step, so a re-export writes the
  same codes.
- **Kept as is.** Per-bone priorities, constraints, and joints that are not in the skeleton. The joints
  that do not match are listed in the import report and written back on export.

An imported file that you export without editing the animation is written back byte for byte, even
when VATs would encode it differently. Props and export settings do not count as edits; **Export
mirrored** does.

## Troubleshooting

### Import failed: not a valid SL animation

The file breaks one of the rules the viewer applies when it loads an `.anim`, for example a duration
over 60 s, a joint named `mRoot`, a truncated file or a value that is not a finite number. The message
names the first problem.

### The import notes say the key times fit no frame rate

The file was made with continuous times, not frames. VATs imports it at 30 fps with keys on fractional
frames, so they play at the times stored in the file.

## See also

- [[Project file format]], VATs' own `.vat` file
- [Second Life Wiki: Internal Animation Format](https://wiki.secondlife.com/wiki/Internal_Animation_Format)

Category: Reference
