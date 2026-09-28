# Face animation

The **Face** window animates the Bento face bones by hand: sliders for the 52 ARKit face shapes and the VRM
presets, a layer that adds blinks, eye darts and a look-at target, and a tool that turns the head or eyes
towards a target. Everything is written as ordinary bone keys through the same face table as
[[Face tracking]].

> Related articles: [[Face tracking]], [[Pose library]], [[Couples and groups]], [[Keys and timeline]]

## Usage

Open the window with **Tools → Face...**. It has three sections: **Expression**, **Blinks, Eye Darts and
Look-At**, and **Look At**.

> **Note:** Face bones only show in Second Life on a mesh head rigged to the Bento face bones.

### Setting an expression

1. Move the playhead to the frame to key.
2. Open a group (**Eyes**, **Brows**, **Cheeks**, **Nose**, **Jaw**, **Mouth**, **Tongue**, **VRM presets**)
   and drag a slider (0–1). **Filter shapes...** shows only the shapes whose name contains the text.
3. The face bones the slider moves are keyed at the frame while you drag. One drag is one undo step.

Only the bones the slider moves get keys, each moved by the slider's change from the value it has now. Bones the
slider does not touch keep their keys. **Reset Face** keys every face bone at rest at the frame.

The project stores bone keys only, never slider values. When you change frame, undo, or edit the face bones
another way, the sliders are read back from the keys (see [[#Reading sliders back]]).

### Saving a face pose

Type a name under the sliders and click **Save Face Pose**. The pose goes to **Inventory → Poses** with the kind
`face`, and applies like any pose, mirrored too (see [[Pose library]]). It holds every face bone's rotation at the
frame and, with **Move face bones** on, the offsets of the bones the table moves.

### Blinks, eye darts and a look-at target

The layer is off until you click **Add Layer**. It changes no keys until you bake.

1. Set the blinks, the eye darts and the look-at target (see [[#Configuration]]).
2. Click **Bake**. The eyes (`mEyeLeft`, `mEyeRight`, `mFaceEyeAltLeft`, `mFaceEyeAltRight`), the eyelids and,
   with a look-at target and **Head turns** above 0, `mHead` get keys on every frame, reduced to within
   0.1°. One undo step.
3. **Re-bake** starts again from the keys those bones had before the first bake, so it can be run after
   changing any setting. **Clear** puts those keys back and removes the layer.

The layer is saved with the project. **Seed** decides the random pattern: the same seed always bakes the same
blinks and darts. **New Seed** picks another.

### Looking at something

**Look At** keys the selected bones to face a target on every frame of **Frames**, like **Follow Target**.

1. Select the head, the eyes, or both. Select the head and the eyes together to turn both; the head is
   turned first.
2. Choose the target under **Look at** and set **Max turn** and **Weight**.
3. Click **Look at Target**. One undo step.

Each bone's forward axis turns towards the target from its animated rotation, keeping its roll. With
**Weight** 1 it points straight at the target, within 1°, unless **Max turn** stops it first.

### Look at partner

In a [[Couples and groups|couple or group]], **Tools → Actors (Couples and Groups)...** has **Look at Partner**
under **Contact with another actor**. It keys the edited actor's head and eyes to look between the eyes of
the actor chosen in **Other actor**, on every frame: the head turns half-way (at most 60°), the eyes the rest
(at most 30°). One undo step.

## Configuration

### Head and Move face bones

| Setting | Default | Effect |
|---|---|---|
| **Head** | **SL default head** | the face table the sliders, the layer and [[Face tracking]] use |
| **Move face bones** | off | also key face-bone offsets; the same setting as in **Motion Capture** |

With **Move face bones** off, shapes that only move bones (most brow, cheek, smile and lip shapes) are greyed
out, because they have nothing to key.

VATs ships one head, **SL default head** (`data/retarget/face-arkit.json`). Your own heads are JSON files in
the `faces` folder of the data folder (see [[Projects and files#Data folders]]):

- **New Head** copies the current head's table into that folder as `my head.json` (`my head 2.json`, ...)
  and chooses it.
- **Folder** shows the folder.
- **Reload** reads the folder and the chosen table again after you edit them.

The format is the one described in [[Face tracking#The face table]].

> **Note:** In the viewer, when your mesh head has its own face joint positions, **Move face bones** is on
> and **Bake shape** is not **Your avatar**, the window warns that the moves will pull the head towards the
> default face. See [[Export to Second Life#Your avatar]].

### Layer

| Setting | Range | Default | Effect |
|---|---|---|---|
| **Seed** | 0 and up | 1 | the random pattern |
| **Blinks** | on/off | on | blink with both eyes |
| **Every** | 0.5–20 s | 2–6 s | the time from one blink to the next, at random in the range |
| **Blink length** | 0.1–0.6 s | 0.25 s | from the lids starting to close to open again |
| **Eye darts** | on/off | on | saccades: quick jumps of the eyes between still moments |
| **Hold** | 0.2–4 s | 0.8 s | the typical time the eyes rest between darts |
| **Eye limit** | 1–30° | 10° | no dart takes the eyes further than this from where they look |
| **Look at** | Nothing, A point, A prop, The camera, Another actor | Nothing | the target |
| **Head turns** | 0–1 | 0.30 | the share of the turn towards the target the head takes; the eyes turn the rest |
| **Head limit** | 0–90° | 45° | the head turns at most this far from straight ahead |

A blink closes the lids over the first 30 % of its length, holds them shut for 15 %, and opens them over the
rest. It uses the table's `eyeBlinkLeft` and `eyeBlinkRight` shapes at full weight, so a head with its own table
blinks by its own amounts. The lids also follow the eyes' pitch by the table's eyelid fractions.

Eye darts follow Lee, Badler and Badler, "Eyes Alive" (SIGGRAPH 2002): the size of a dart falls off
exponentially (a mean of 6.9°), and a dart of A degrees lasts 25 ms + 2.4 ms × A. The hold between darts is
log-normal around **Hold**. Darts go straight up, down or sideways twice as often as diagonally.

Towards a look-at target the eyes turn at most 30° from straight ahead.

### Looping clips

With **Loop** on, the frames before loop-in and the loop itself are generated separately. Each ends with the
eyes back where they look and no blink running, and blinks stay at least the shortest **Every** time apart
across the loop's seam, so the loop joins cleanly. Frames after loop-out get no blinks or darts.

### Targets

| Target | Where it is |
|---|---|
| **A point** | **Point**, in avatar space (X forward, Y left, Z up, metres). **Use the Selected Bone's Position** takes the selected bone's position at the current frame. |
| **A prop** | the origin of the chosen prop; a prop on a bone moves with the animation |
| **The camera** | where the camera is when you bake |
| **Another actor** | **Their bone** of the chosen actor, or between their eyes, on every frame |

### Look At tool

| Setting | Range | Default | Effect |
|---|---|---|---|
| **Frames** | 0 to the last frame | the whole clip | the frames keyed |
| **Max turn** | 0–90° | 60° | each bone turns at most this far from straight ahead |
| **Weight** | 0–1 | 1 | how far from the animation towards the target |

## Reading sliders back

The sliders show the shape weights (0–1) whose keys come closest to the face bones' keys at the frame, found by
a least-squares fit that prefers fewer shapes. Limits:

- Shapes that move the same bones the same way cannot be told apart; the sliders show one of the combinations.
- **VRM presets** read back as the ARKit shapes they stand for, so a preset slider returns to 0 once the frame
  changes.
- With **Move face bones** off, shapes that only move bones read back as 0.
- Keys no combination of shapes makes, for example an eyelid turned by hand past a shape's range, read back as
  the nearest combination. Moving a slider then moves those bones by the slider's change, without a jump.
- While the sliders still give the keys at the frame, they stay as you set them.

## Tips and tricks

- Bake the layer last, after the head animation is done: re-baking starts from the keys before the first bake,
  so head keys set after a bake with a look-at target are replaced.
- For a couple, use **Look at Partner** for a steady gaze, or the layer with **Another actor** as the target
  for a gaze with blinks and darts.
- Save the expressions you use often as face poses and apply them at the frames you need.

## Troubleshooting

### A slider is greyed out

The shape only moves face bones. Turn on **Move face bones**, or use a shape that turns bones.

### The window shows "my head.json: ..."

The head's table could not be read. Fix the JSON and click **Reload**, or choose **SL default head**.

### The eyes do not look at the target

The eyes turn at most 30° from straight ahead. Raise **Head turns**, or use **Look At** on the head first.
Another actor's target also needs that actor in the scene; with no actor chosen the layer bakes looking ahead
and says so.

## See also

- [[Face tracking]]
- [[Pose library]]
- [[Couples and groups]]
- [[Project file format#face_layer]]

Category: Animating
