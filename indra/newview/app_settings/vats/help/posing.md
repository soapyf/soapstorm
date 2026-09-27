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
- **Select → Select All** (**Ctrl+A** in the Industry preset), **Select Keyed on Frame** (**Ctrl+Shift+A**), **Select All Keyed** and **Select None** select in bulk.

### Moving and rotating

Pick a tool from the timeline bar or the **Tools** menu. The keys below are the Industry preset; see [[Keyboard shortcuts]] for the others.

| Tool | Key | What it does |
|---|---|---|
| **Select Tool** | **Q** | Select only, no gizmo |
| **Move Tool** | **W** | Arrows move along one axis, squares in a plane, the centre in the view plane |
| **Rotate Tool** | **E** | Rings turn around one axis, the outer ring around the view, the inside of the ball freely |
| **Scale Tool** | **R** | Scales static props only; Second Life animations can't scale bones |

- **Tools → Cycle Local / World / Gimbal Axes** (**O**), or the button next to the tools, sets the gizmo's axes. **Gimbal** shows the three rotation channels as they are stored.
- With the Rotate tool you can drag a bone directly: it turns around the direction you are looking.
- Hold **Ctrl** while dragging to snap rotations to the step set in [[Preferences]] (**Rotation snap**, 5° by default). In the Second Life preset, **G** turns snapping on and off instead.
- **Esc** or a right-click during a drag puts everything back.

### Typing exact values

The **Bone** section of **Properties** shows the primary bone's **Rotation** in degrees around X, Y and Z; type or drag to change it. Under it, **Keyed at this frame** or **Not keyed at this frame** tells you whether the values are a key or are interpolated.

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

- [[Keys and timeline]]
- [[Interface]]
- [[Keyboard shortcuts]]

Category: Animating
