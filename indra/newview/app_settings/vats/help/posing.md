# Posing

Posing is setting the rotation (and sometimes the position) of bones at the current frame. Every move or turn you make in the view is keyed at that frame straight away, so posing and keying are the same step.

> Related articles: [[Skeleton]], [[Keys and timeline]], [[IK]], [[Hand poser]], [[Control presets]]

## Usage

### Selecting bones

- **Click** a bone in the view to select it. Its name shows in **Properties**.
- **Click the same spot again** to reach a bone hidden underneath the first.
- **Shift+click** adds a bone to the selection or removes it. The last bone picked is the *primary* bone: the gizmo sits on it and its keys are drawn brighter on the timeline.
- **Esc** clears the selection (in the Second Life preset, **Esc** resets the camera instead).
- **Up** / **Down**, or **[** / **]**, walk to the parent or child bone (**Select → Select Parent**, **Select Child**). **Select → Next Sibling** and **Previous Sibling** step sideways.
- In the **Bones** tab, click a name. Picking a bone in the view opens the list at that bone.
- In the **Picker** tab beside it, click a body part on the avatar outline; **Shift+click** adds one. Save a selection you use often as a selection set there. See [[Picker]].
- **Select → Select All** (**Ctrl+A** in the Industry preset), **Select Keyed on Frame** (**Ctrl+Shift+A**), **Select All Keyed** and **Select None** select in bulk.

### Moving and rotating

Pick a tool from the timeline bar or the **Tools** menu. The keys below are the Industry preset; see [[Keyboard shortcuts]] for the others.

| Tool | Key | What it does |
|---|---|---|
| **Select Tool** | **Q** | Select only, no gizmo |
| **Move Tool** | **W** | Arrows move along one axis, squares in a plane, the centre in the view plane |
| **Rotate Tool** | **E** | Rings turn around one axis, the outer ring around the view, the inside of the ball freely |
| **Scale Tool** | **R** | Scales static props only; Second Life animations can't scale bones |

![The Rotate tool's three coloured rings around the head bone, with the outer view ring](images/posing/rotate-gizmo.png)
*The Rotate tool on **mHead**: the red, green and blue rings turn around X, Y and Z; the pale outer ring turns around the view.*

- **Tools → Cycle Local / World / Gimbal Axes** (**O**), or the button next to the tools, sets the gizmo's axes. **Gimbal** shows the three rotation channels as they are stored.
- With the Rotate tool you can drag a bone directly: it turns around the direction you are looking.
- Hold **Ctrl** while dragging to snap rotations to the step set in [[Preferences]] (**Rotation snap**, 5° by default). In the Second Life preset, **G** turns snapping on and off instead.
- **Esc** or a right-click during a drag puts everything back.

### Typing exact values

The **Bone** section of **Properties** shows the primary bone's **Rotation** in degrees around X, Y and Z; type or drag to change it. Under it, **Keyed at this frame** or **Not keyed at this frame** tells you whether the values are a key or are interpolated.

![The Bone section of Properties: mHead, Rotation 0.0, 15.0 and 0.0 degrees, Keyed at this frame](images/posing/bone-properties.png)
*The **Bone** section on a keyed frame. Type into a field to key that value.*

A bone that normally only rotates can also move: press **Animate Position**, then set **Offset (m)**. Positions are in metres.

### Per-bone priority

The **Priority** box in the **Bone** section overrides the animation's priority for this bone alone in the exported `.anim`. **Clip (N)** means the bone uses the animation's priority N; 0–6 sets its own. Higher wins over other animations. See [[Animation priority]].

### The right-click menu

Right-click a bone for its menu:

- **Select**, **Key** and **Reset** the bone.
- The same for its body part (for example **Key Left Arm**), plus **Mirror** it to the other side, **Copy** and **Paste onto** it, and save it as a pose or, with a frame range picked, as a clip.
- **Show Hand Poser**, the IK switch, and the pin commands of [[Hold and bind]].

Right-click empty space for selection commands, **Copy Pose**, **Paste Pose**, **Save Pose...** and your saved **Poses**.

### Resetting and copying

| Command | Key (Industry) | What it does |
|---|---|---|
| **Edit → Reset Selected Bone** | **Alt+R** | Returns the selected bones to their rest pose at this frame |
| **Edit → Reset Hip Position** | **Alt+W** | Keys the hips back to their rest position (only when the hips have position keys) |
| **Edit → Reset Whole Pose** | **Alt+Shift+R** | Resets every bone at this frame |
| **Edit → Copy Pose** | **Ctrl+C** | Copies the pose of the selected bones, or the whole pose when nothing is selected |
| **Edit → Paste Pose** | **Ctrl+V** | Pastes it at the current frame |

For mirroring, see [[Mirror, flip and reverse]].

### Worked example: keying a head turn

[Open the example](example:posing-head-turn.vat): the Relaxed Stand pose held for 24 frames, with **mHead** keyed straight ahead (`0`, `0`, `0`) at frame 0 and nowhere else.

1. Type `12` in the **Frame** box on the timeline bar.
2. Click **mHead** in the **Bones** tab. **Properties** says **Not keyed at this frame**.
3. In **Properties → Bone**, set **Rotation** to `0`, `0`, `30`.

The head turns to the avatar's left, **Properties** now says **Keyed at this frame**, **mHead** turns amber in the **Bones** tab, and a second diamond sits on frame 12 of the timeline. Scrub from 0 to 12: the head turns smoothly from the straight-ahead key to the new one. From 12 to 24 it stays turned: the last key holds.

> **Note:** A bone's only key sets its pose on every frame, before it as well as after. Key the starting pose first when a move should start from rest.

### Mirror while posing

The **Mirror** button on the timeline bar, next to **IK / FK**, turns live mirroring on and off. While it is on, every gizmo drag (including a direct drag on a bone and the Blender preset's **G** and **R**), every [[Hand poser]] drag or double-click, and every move or turn of an [[IK]] target or pole also keys the partner on the other side, at the same frame, with the mirrored pose. The partner's IK / FK state stays its own.

While it is on, the status bar shows **Mirror on** in purple and the gizmo's centre and outer ring are purple. It is off each time VATs starts.

- A partner gets position keys where either side has them, and always when it is an attachment point.
- A bone in the middle, such as the head or the spine, is its own partner. It poses as usual unless **Mirror centre bones in place** is on in [[Preferences]]: then it is kept symmetric, halfway between your pose and its mirror image, so a nod stays and a turn or sideways lean is cancelled.
- Typing values in **Properties**, pinned points and the QAvimator preset's modifier drags are not mirrored.

### Scratch pose

**Edit → Scratch Pose** turns scratch posing on and off (a tick shows it is on). While it is on, your edits change the pose you see but write no keys to the document: the status bar says "Scratch Pose: nothing is keyed until Set Key", and the **Bones** tab marks each changed bone **(scratch)**. **Undo** steps back through the scratch edits.

- **Set Key** (**S** in the Industry preset) keeps the pose: every channel you changed gets a key at the scratch frame, as one undo step, **Key Scratch Pose**.
- Moving to another frame (scrubbing, playing, stepping to a key), or turning **Scratch Pose** off, asks **Keep scratch pose as keys?**: **Keep**, **Discard** or **Cancel**. **Don't ask again** remembers the answer; **Leaving a scratch pose** in [[Preferences]] asks again.
- With **Only key channels that already have keys** on in [[Preferences]], keeping the pose keys only channels that had keys before, so a bone that was never animated stays unkeyed.
- Saving, exporting, uploading and autosave write the document without the scratch pose.
- Switching to another actor, or a change to the whole scene (the actors, or the frame rate), discards the scratch pose.
- Only the pose at the scratch frame is kept: keys moved in the [[Graph editor]] during a scratch pose are not.

### Propagating a pose

**Edit → Propagate Pose** writes the current pose of the selected bones (with their pin offsets and any selected IK controls) onto their later keys, as one undo step:

| Command | Keys it changes |
|---|---|
| **To Next Key** | Each bone's next key after this frame |
| **To Selected Range** | Every key after this frame inside the frame range picked on the timeline (**Shift+drag**) or spanned by the keys selected in the [[Graph editor]] |
| **To End** | Every key after this frame |

It adds no keys, and keeps each key's frame, interpolation and handles. An IK control's IK / FK state is not changed. With a scratch pose, the scratch pose is keyed first. The status bar says how many keys changed, or "No later keys to change".

## Configuration

Mouse and key behaviour depends on the control preset (Industry, Blender, QAvimator or Second Life), set under **Navigation & hotkeys** in **Edit → Preferences...**. In the Blender preset, **G** and **R** over the view start a Blender-style move or rotate; **X**, **Y** or **Z** lock an axis. In the Second Life preset the Move gizmo is the default, **Ctrl** switches to Rotate and **Ctrl+Shift** to Scale. See [[Control presets]].

## Tips and tricks

- Place hands and feet with [[IK]] instead of rotating each joint.
- Curl fingers with the [[Hand poser]], or apply a starter hand shape from the [[Pose library]].
- **View → Bones in Front (X-ray)** makes bones inside the body clickable.

## Troubleshooting

### Reset Hip Position does nothing

The status bar says "The hip has no position keys": the hips were never moved, so there is nothing to reset. **Alt+W** only acts on hips that have position keys.

### A bone jumps when I scrub past a frame

The bone has a key there that you did not mean to set: moving a bone always keys it. Delete the key (**Delete**) or open the [[Graph editor]] to see the curve.

## See also

- [[Picker]]
- [[Keys and timeline]]
- [[Interface]]
- [[Keyboard shortcuts]]

Category: Animating
