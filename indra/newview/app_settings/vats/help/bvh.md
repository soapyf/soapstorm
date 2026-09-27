# BVH

BVH (Biovision Hierarchy) is a text format for skeletal motion that most animation tools and the Second
Life viewer's upload can read. VATs imports BVH files that use SL joint names and exports BVH for other
tools. For BVH files from other rigs, such as Mixamo or motion-capture suits, use [[Retargeting]]
instead.

> Related articles: [[Anim format]], [[Export to Second Life]], [[Retargeting]], [[Skeleton]]

## Usage

### Import

Choose **File → Import BVH...**. The file must use SL joint names, with `mPelvis` (or an alias the
viewer accepts, such as `hip`) as the root. The import replaces the current clip, marks the project
untitled and modified, and keeps its props.

- **Names.** Each joint is matched by its exact name or a viewer alias. If that fails, VATs tries the
  name without regard to case and reports the match in **Import notes**. Joints that match nothing are
  skipped and listed.
- **Channels.** Any channel order and any number of channels per joint are read. Positions are in
  inches.
- **Frames.** When the file has more than one frame, frame 0 is the reference frame: the hip's rotation
  and position are measured from it, and it is not imported. The clip has `Frames − 2` as its last
  frame, and every remaining frame becomes a linear key.
- **Frame rate.** `1 / Frame Time`, rounded and limited to 1–120 fps.
- **Playback settings.** Priority 2, ease in and ease out 0.3 s, **Loop** off.

### Export

**File → Export BVH (Animated Bones)...** writes the animated bones and every parent up to the hip.
**File → Export BVH (All Bento Bones)...** writes every bone of the skeleton, keyed or not. The export
uses the name, folder and mirror settings of [[Export to Second Life]], with the `.bvh` extension.

The file VATs writes:

- joint names are SL names, and the root is `mPelvis`;
- the hip has six channels, `Xposition Yposition Zposition Zrotation Xrotation Yrotation`; other bones
  have three, `Zrotation Xrotation Yrotation`;
- frame 0 is the reference frame (rest pose, no rotation), followed by one frame for each clip frame, so
  `Frames` is the last frame plus 2;
- `Frame Time` is `1 / fps`;
- IK, pins and dynamics are baked against the export **Bake shape**;
- **Export mirrored (left and right swapped)** applies.

### What BVH loses

Before it writes a BVH, VATs lists what the file cannot carry, in a **BVH loses some of this**
dialog:

- `attachment point motion (X)`;
- `position keys on X`, for bones below the hip, unless **BVH: include bone positions** is ticked;
- `per-joint priorities`;
- `constraints`;
- `imported joints that are not in the skeleton`.

Choose **Export .anim Instead**, **Export BVH Anyway** or **Cancel**. BVH also has no priority, loop,
ease, hand pose or expression; you set these in the viewer's upload window.

## Configuration

| Setting | Default | Where |
|---|---|---|
| Reduce keys after import | off | **Edit → Preferences... → BVH import** |
| BVH: include bone positions | off | **Properties → Export** |

**Reduce keys after import** drops keys that linear playback reproduces within 0.05 degrees and 0.5 mm.
Off keeps a key on every frame.

**BVH: include bone positions** writes position channels for moved bones other than the hip. Many tools
expect rotation only below the hip, so it is off by default.

## Format

A BVH file has two parts:

```
HIERARCHY
ROOT mPelvis
{
	OFFSET ...
	CHANNELS 6 Xposition Yposition Zposition Zrotation Xrotation Yrotation
	JOINT ...
}
MOTION
Frames: 32
Frame Time: 0.033333
...
```

Axes differ from SL: BVH X is SL Y, BVH Y is SL Z (up), and BVH Z is SL X (forward). Lengths are in
inches; VATs converts at exactly 0.0254 m per inch.

## Troubleshooting

### Import failed: the root joint is not the hip (mPelvis)

The file comes from another rig. Import it with **File → Import Animation (Retarget)...**; see
[[Retargeting]].

### Import failed: the file has no motion channels

No joint in the file has a `CHANNELS` line with channels. The file holds a skeleton only.

### Bones stay still after import

Their names match no SL joint and were skipped; **Import notes** lists them. Rename the joints in the
source tool, or use [[Retargeting]].

### The upload in the viewer looks different from VATs

The BVH lost something listed in [[BVH#What BVH loses]], or the viewer's upload applied its own priority,
ease and loop settings. Export an `.anim` instead; it keeps everything.

## See also

- [Second Life Wiki: BVH](https://wiki.secondlife.com/wiki/BVH)

Category: Import and export
