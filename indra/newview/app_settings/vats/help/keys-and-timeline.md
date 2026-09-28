# Keys and timeline

An animation is a set of keys: poses stored at particular frames, which VATs interpolates between. The timeline at the bottom of the window shows where the keys are, plays and scrubs the animation, and carries the loop, ease and range markers.

> Related articles: [[Posing]], [[Graph editor]], [[Time editing]], [[Loop tools]], [[Animation priority]]

## Usage

### Setting keys

1. Move the playhead to a frame: click or drag in the timeline, type in the **Frame** box, or use **Left** / **Right**.
2. Pose the avatar. Moving or rotating a bone keys it at that frame; there is nothing else to press.
3. Move to another frame and pose again.

Other ways to key (Industry keys; see [[Keyboard shortcuts]] for the other presets):

| Command | Key | What it does |
|---|---|---|
| **Edit → Set Key** | **S** | Keys the selected bones, pins and IK controls as they are now |
| **Edit → Set Key on All Visible Bones** | **Shift+S** | Keys every bone that is shown |
| **Edit → Tween (Breakdown)** | **Shift+E** | Keys an in-between pose; see [[Keys and timeline#Tweening between keys]] |
| **Edit → Delete Key** | **Delete**, **Backspace** | Deletes the selected bones' keys at this frame |
| **Edit → Delete Keys on All Bones at Frame** | **Shift+Delete** | Deletes every bone's key at this frame |

The **Set Key** button on the timeline bar does the same as **S**. In the Blender preset, **I** sets a key and **Alt+I** deletes one.

### Tweening between keys

The **Tween** slider on the timeline bar, right of **Set Key**, keys the selected bones at the playhead with a pose part of the way between their previous key and their next key: **0%** is the previous key's pose, **100%** the next key's, and the slider runs from **-20%** to **120%** to push past either key. Each bone uses its own neighbouring keys.

- Rotations turn along the shortest arc between the two poses (quaternion slerp), then take the Euler angles closest to the curve at the playhead, so a curve that has turned several times stays on its turn.
- Positions, and the pins' offsets, move in a straight line.
- A limb in IK is keyed through its IK control: with a bone of that limb selected, or its IK control, the control's target and pole are tweened instead of the bones. The IK/FK switch itself is not changed.
- A bone with no key before or after the playhead is left alone, and the status bar says so.

Tick **Relax** next to the slider to work on existing keys instead: the slider then pulls the selected bones' keys at the playhead toward the curve their neighbouring keys would make without them. **0%** leaves the key as it is and **100%** puts it on that curve. It needs a key at the playhead and another key on the same bone.

Instead of the slider, press **Shift+E** and move the mouse left or right; the bottom left of the viewport shows the amount. Hold **Ctrl** for 10% steps. A left click, **Enter** or **Space** keys it; a right-click, **Esc** or **Ctrl+Z** cancels. The drag starts from the slider's last value.

Each use, a slider drag or a **Shift+E** drag, is one undo step, **Tween**. **Ctrl+click** the slider to type a value.

### Reading the timeline

- A faint tick marks every frame that has a key on any bone.
- A diamond marks each key of the selection (bones, their pins and IK controls). The primary bone's diamonds are brighter and larger.
- In the **Bones** tab, a bone with a key on this frame is amber, and a bone animated anywhere is tan.
- **Properties → Bone** says **Keyed at this frame** or **Not keyed at this frame** for the primary bone.

![The timeline at frame 8 of a 24-frame animation: key diamonds at 0, 8 and 16, a loop band from 0 to 16 with a flag at each end, and the ease triangles under the ruler](images/keys-and-timeline/timeline.png)
*The timeline with **mHead** selected: its keys at 0, 8 and 16 as diamonds, the loop band from **Loop in** 0 to **Loop out** 16, and the ease triangles at the ends.*

### Moving through time

| Command | Key (Industry) |
|---|---|
| **Playback → Play / Pause** | **Space** |
| **Playback → Next Frame** / **Previous Frame** | **Right** / **Left** |
| **Playback → Next Key** / **Previous Key** | **.** / **,** |
| **Playback → Go to Start** / **Go to End** | **Home** / **End** |

**Next Key** and **Previous Key** jump between the keys of the selection. The same commands sit on the timeline bar as icons; hover one for its name and key, or see [[Interface#Timeline]] for what each icon looks like.

### Length and frame rate

In **Properties → Animation**:

- **Frame rate** is frames per second, 1–120 (30 by default). When you change it, VATs asks what to keep:
  - **Keep Frame Numbers**: keys stay on their frames, so the animation plays faster or slower.
  - **Keep Timing**: keys move so everything happens at the same second.
- **Last frame** sets the length, 0–3600 (30 by default). The length in seconds is shown underneath, in red with "over SL's 60 s limit" past 60 seconds.

### Looping

Tick **Loop** in **Properties → Animation**, or press the **Loop** button on the timeline bar (two arrows chasing each other, after **Go to end**; highlighted while looping). The loop is the frames between **Loop in** and **Loop out**, shown as two flags on the timeline:

- Drag a flag to move it. Dragging a flag also turns **Loop** on.
- **Alt+drag** inside the loop band moves both flags together.

Second Life plays the part before **Loop in** once, then repeats the loop. For tools that fix loop seams and walk cycles, see [[Loop tools]].

### Easing in and out

**Ease in** and **Ease out** (seconds, 0–10 in 0.05 s steps) blend the animation in from whatever the avatar was doing, and out again at the end. Type them in **Properties → Animation**, or drag the small triangles at the ends of the timeline.

### Picking a frame range

**Shift+drag** along the timeline to mark a range; it shows as a yellow band. A plain click clears it. Ranges are used by [[Time editing]], **Edit → Save Clip of Selected Bones...** ([[Pose library]]) and the graph's keys.

### Priority, hand pose and expression

The rest of **Properties → Animation** sets values stored in the exported `.anim`:

- **Priority** (0–6): see [[Animation priority]]. Above 4 the panel warns that some viewers treat it as 4.
- **Hand pose**: one of Second Life's built-in hand shapes, which the viewer applies on top of the animation. This is not the same as posing the finger bones; see [[Hand poser]].
- **Expression**: one of Second Life's built-in facial expressions.

### Worked example: a three-key nod

[Open the example](example:keys-head-nod.vat): 24 frames at 30 fps, **Loop** ticked from 0 to 16, and **mHead** keyed at frames 0, 8 and 16 with **Rotation** Y at 0°, 15° and 0°.

1. Click **mHead** in the **Bones** tab. Its three keys show as diamonds on the timeline.
2. Press **.** (**Next Key**) twice: the playhead jumps from 0 to 8, then 16. At frame 8, **Properties → Bone** reads **Rotation** `0.0°`, `15.0°`, `0.0°` and **Keyed at this frame**.
3. Type `4` in the **Frame** box: **Rotation** reads `7.5°` on Y and **Not keyed at this frame**. That value is interpolated, halfway between the keys either side.
4. In **Properties → Animation**, set **Last frame** to `32`: the line under it reads **1.07 seconds**, and the ruler grows to 32; the keys and the loop flags stay where they were.

## Tips and tricks

- **Select → Select Keyed on Frame** selects the bones keyed at the current frame, so **S** re-keys just those.
- Block out with **Stepped** keys in the [[Graph editor]], then switch to **Auto** when the timing works.
- Scrubbing with audio loaded plays short snippets of the sound; see [[Audio track]].

## Troubleshooting

### The animation is refused by Second Life for being too long

Second Life rejects animations longer than 60 seconds. The seconds under **Last frame** turn red past that; shorten the clip or lower the frame count.

### Moving a loop flag turned looping on

That is intended: a loop flag only means something with **Loop** ticked. Untick **Loop** to turn it off again; the flags keep their frames.

## See also

- [[Graph editor]]
- [[Onion skin]]
- [[Export to Second Life]]

Category: Animating
