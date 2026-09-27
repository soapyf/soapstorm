# Export to Second Life

VATs writes Second Life animations as `.anim` files, the viewer's own binary format, and as BVH for
other tools. An `.anim` keeps everything VATs can animate: attachment points, moved bones, per-bone
priorities and constraints. You upload the file from any Second Life viewer, or from inside the viewer
with the [[VATs Editor (viewer)]].

> Related articles: [[Animation priority]], [[Anim format]], [[BVH]], [[Couples and groups]], [[Mesh bodies]]

## Usage

### Export an .anim

1. Set the clip's length, loop, priority and ease in **Properties → Animation** (see
   [[Animation priority]]).
2. Choose **File → Export SL .anim...** (**Ctrl+E**). The **Export SL .anim** dialog opens. The same
   settings are also in **Properties → Export**.
3. Check the **Saves as** line, then press **Export SL .anim**.

The first export asks for a folder when none is set (**Folder** shows `(asks the first time)`). After
that, **Export SL .anim** writes straight to the folder; **Choose...** changes it. The status bar reports
the files written, how many were replaced, and a summary of bones, length, priority, bytes, and any
attachment points that move or rotate.

The export settings are saved with the project, and each change to them is an undo step.

### Name the files

| Field | Meaning |
|---|---|
| **Name** | The animation name. Empty uses the project file name, or `Animation` for an unsaved project. |
| **Number** | 0–999, written with at least two digits (`01`). |
| **Side** | **(none)**, **Left** or **Right**. |
| **Pattern** | Default `[NAME]_[#]_[SIDE]`. Tokens: `[NAME]`, `[#]`, `[SIDE]`, `[ACTOR]`. |

VATs removes the characters `\ / : * ? " < > |`, collapses doubled separators (`__` becomes `_`) and
trims separators from the ends, so an empty side leaves no stray underscore: `Wave_01.anim`.

- **Also export the other side (mirrored)** writes a second file with the sides swapped:
  `Wave_01_Left.anim` and `Wave_01_Right.anim`. With **Side** at **(none)**, the mirrored file gets
  `_mirrored` appended.
- **Count the number up after each export** adds 1 to **Number** after each successful `.anim` export,
  so the next export does not replace the last one.
- **Export mirrored (left and right swapped)** swaps the sides in the exported file only; the project is
  unchanged.

### Choose the bake shape

IK, pins and dynamics are baked into plain keys at export. **Bake shape** sets the body they are baked
against, whatever the view shows:

- **SL Default** and **SL Default (Male)**;
- **Mesh body:** and the body name, for each body in the inventory. A mesh body uses the joint positions it was
  rigged to (see [[Mesh bodies]]).

### Reduce keys

VATs samples every bone at every whole frame, then removes keys that the viewer's interpolation
reproduces within a tolerance. **Reduce keys** has two fields: rotation (default `0.050 deg`) and
position (default `0.50 mm`). The first and last frames and the frames where you set keys are always
kept. Set both fields to `0` to keep a key on every frame.

### Export BVH

**File → Export BVH (Animated Bones)...** writes the animated bones and their parents; **File → Export BVH
(All Bento Bones)...** writes every Bento bone, keyed or not. The **Export** section has the same two
buttons under the same names. BVH cannot carry attachment points,
per-bone priorities or constraints; see [[BVH#Export]].

### Upload

In a standard Second Life viewer, choose **Build → Upload → Animation**, pick the `.anim` file, and
confirm the fee. In SoapStorm, the [[VATs Editor (viewer)]] uploads directly with **File → Upload Animation...**:
every file Export would write, each with the viewer's price confirmation.

> **Warning:** Uploading costs L$ and cannot be undone. Test on the Aditi beta grid, where uploads are
> free, or preview the animation in the viewer first.

## Configuration

| Setting | Default | Where |
|---|---|---|
| Pattern | `[NAME]_[#]_[SIDE]` | **Properties → Export** |
| Number | `1` | **Properties → Export** |
| Bake shape | **SL Default** | **Properties → Export** |
| Reduce keys | `0.05` degrees, `0.5` mm | **Properties → Export** |
| BVH: include bone positions | off | **Properties → Export** |

All of these are stored in the project, not in the preferences.

## Troubleshooting

### Export refuses the animation

VATs checks each file with the same rules the viewer uses when it reads an `.anim`. A file the viewer
would reject gets a **Cannot export** message and is not written:

- `the animation is longer than 60 seconds; SL will not play it` (**Properties → Animation** already
  shows `over SL's 60 s limit`);
- `the file is N bytes; SL accepts animations under 250000 bytes`;
- `nothing is keyed`.

Shorten the clip, raise **Reduce keys**, or remove keys from bones that do not move. For long motion
capture, [[Retargeting#Fit SL's limits]] can split the clip into parts.

### Exported with warnings

The file is written, and the message lists what the viewer may do differently. Examples: `loop in is
after loop out`, `some positions are further than 5 m and were clamped`, and `track "X" matches no bone
and is not exported`.

### A bone does not play in-world

Another animation with a higher priority controls that bone. Raise the priority of the clip or of that
bone; see [[Animation priority]].

### Ease values are ignored

With **Loop** off, an ease in plus ease out longer than the animation gives the warning `ease in + ease
out is longer than the animation`. The `.anim` keeps the values as written; a BVH upload scales both
down to fit. Shorten the ease times.

### The mesh body poses differently in-world

The animation was baked against a different shape. Set **Bake shape** to the body you wear.

## See also

- [[Anim format]], the byte layout VATs writes
- [Second Life Wiki: Animation](https://wiki.secondlife.com/wiki/Animation)
- [Second Life Wiki: Aditi](https://wiki.secondlife.com/wiki/Aditi), the beta grid

Category: Second Life
