# Graph editor

The graph editor shows the curves behind the keys: how each rotation and position channel of a bone changes over time. Use it to shape the motion between keys, fix timing, and change how keys ease in and out.

> Related articles: [[Keys and timeline]], [[Posing]], [[Hold and bind]], [[Control presets]]

## How the curves work

### Channels and units

Every animated bone has up to six curves, one per channel. Each curve is a list of keys (a frame and a value), and the curve between the keys is what the bone does.

- **Rotate X**, **Rotate Y** and **Rotate Z** are angles in degrees, relative to the bone's rest pose. They are applied X first, then Y, then Z, about the parent's axes. A bone with no keys on a channel reads 0 there.
- **Translate X**, **Translate Y** and **Translate Z** are offsets from the bone's rest position, in metres.
- **IK / FK Blend** runs from 0 to 1. At 0.5 or more the limb follows its [[IK]] target. Its keys are **Stepped**, so the limb switches cleanly on a frame.
- **Pole X**, **Pole Y** and **Pole Z** place the IK pole, the point the elbow or knee aims at.

The graph draws X in red, Y in green and Z in blue; the pole curves use lighter shades of the same colours, and **IK / FK Blend** is white. The frame axis runs along the top. Frames before 0 and after the last frame are shaded, and a loop range is tinted.

### Posing writes keys to the curves

Rotating a bone in the viewport keys all three rotation channels on the current frame. VATs turns the new rotation into the three angles closest to the curve's current values. This keeps the curves continuous, so a bone turned past 180° reads 190°, not −170°.

Curves imported from other tools may not follow this rule. There a bone can flip by 360° between two keys even though the pose looks the same; **Euler Filter** finds and removes those jumps. When **Rotate Y** is near ±90°, X and Z turn about the same axis (gimbal lock). The pose is still right, but the X and Z curves can look odd there.

### Between and beyond the keys

Each key sets how the curve runs to the next key:

| Segment | Curve |
|---|---|
| **Stepped** | Holds the key's value until the next key |
| **Linear** | A straight line to the next key |
| All other tangent types | A smooth curve shaped by the two keys' handles |

Before the first key a curve holds the first key's value, and after the last key it holds the last key's value.

**Auto**, **Spline**, **Plateau**, **Linear** and **Flat** handles are automatic: VATs recalculates them whenever a neighbouring key moves, is added or is deleted. Dragging a handle freezes it where you leave it. Normally the other handle turns to stay in line; after **Break**, each handle moves on its own. **Unify** lines a broken pair up again and keeps both frozen. To make a frozen handle automatic again, press one of the automatic tangent buttons.

### What reaches Second Life

A `.anim` file does not store curves. On export VATs samples every bone on every whole frame, including [[IK]] and [[Hold and bind|pins]], and Second Life plays straight lines between the keys it keeps. **Reduce keys** in **Properties → Export** drops each key that the straight line through its neighbours already reproduces, within 0.05° and 0.5 mm by default. It always keeps the first frame, the last frame and every frame where your curves have a key, and never leaves more than 60 frames between two keys. See [[Export to Second Life#Reduce keys]].

What this means for the curves:

- The curve's shape between whole frames never reaches SL. A key moved to a fraction of a frame (with **Snap frames** off) plays in VATs, but SL only sees the whole frames around it.
- A smooth ease survives as a run of short straight segments, one per frame at most.
- A **Stepped** key holds exactly: the jump to the next value happens within one frame.

## Usage

### Opening the graph

**View → Graph Editor** (**Ctrl+G**) shows or hides the **Graph** panel.

The list on the left shows the curves of the selected bones. The drop-down above it switches between **Selected bones** and **All animated bones**. Each bone lists its channels:

| Channel | Meaning |
|---|---|
| **Rotate X**, **Rotate Y**, **Rotate Z** | The bone's rotation |
| **Translate X**, **Translate Y**, **Translate Z** | The bone's position: shown for the hips, attachment points and bones with position keys |
| **IK / FK Blend** | For a selected [[IK]] target: whether the limb follows IK or its rotation keys |
| **Pole X**, **Pole Y**, **Pole Z** | For a selected IK pole: where the elbow or knee points |

Click a row to show only that channel; **Shift+click** or **Ctrl+click** adds or removes rows. A pinned point also lists its pin offset curves, marked **(pin)**. With nothing selected the graph reads "Select a bone to see its curves".

### Selecting and moving keys

- **Click** a key to select it. **Shift+click** toggles a key in the selection; **Ctrl+click** removes one. Clicking the same spot again cycles through keys stacked on top of each other.
- **Drag across empty space** to box-select.
- **Drag** selected keys to move them in time and value. Hold **Shift** while dragging to lock the move to one direction.
- **Esc** during a drag cancels it.
- With **Snap frames** ticked, keys stay on whole frames while moving and scaling.
- The **Frame** and **Value** boxes show the earliest selected key; typing a new value moves the whole selection by the same amount.

### Scaling keys

With two or more keys selected, a box with eight handles appears around them. Drag a side handle to stretch the keys in time or in value; drag a corner to do both. This is one undo step (**Scale Keys**).

### Adding and deleting keys

- **Double-click** a curve to add a key on it at that point (**Insert Key**). The curve's shape does not change.
- **Delete** (the button, or **Delete** with the mouse over the graph) removes the selected keys.
- **Ctrl+C** and **Ctrl+V** with the mouse over the graph copy and paste keys; pasting puts them at the current frame.

### Shaping curves: tangents

A selected key shows its handles; drag them to shape the curve. The buttons along the top set the tangent type of the selected keys:

| Button | Curve |
|---|---|
| **Auto** | Smooth, flat at peaks and valleys |
| **Spline** | Smooth through the neighbours; can overshoot |
| **Plateau** | Smooth without ever overshooting |
| **Linear** | Straight towards the neighbouring keys |
| **Flat** | Level handles: eases in and out of the key |
| **Stepped** | Holds the value until the next key |
| **Break** | Lets each handle move on its own |
| **Unify** | Lines both handles up again |

### Fixing and flipping curves

- **Euler Filter** removes 360-degree jumps from the rotation curves shown. When there is nothing to fix, the status bar says "Rotation curves are already clean".
- **Flip Time** mirrors the selected keys in time.
- **Flip Values** mirrors the selected keys across zero.

### Navigating

**Frame All** fits every shown curve, **Frame Selected** fits the selected keys, and **Fit Values** fits only the height. With the mouse over the graph, the view's **Frame Selected** and **Frame All** keys (**F** and **A** in the Industry preset) act on the graph.

The mouse wheel zooms about the cursor; with **Shift** it zooms values only, with **Ctrl** time only. Panning and drag-zooming follow your control preset, and the status bar shows the hint while the mouse is over the graph:

| Preset | Pan | Zoom |
|---|---|---|
| Industry | **Alt+left** or **Alt+middle** drag | **Alt+right** drag, wheel |
| Blender | Middle drag (with **Emulate 3-button mouse**: also **Alt+left** drag) | **Ctrl+middle** drag, wheel |
| QAvimator | Middle drag | Wheel |
| Second Life | **Ctrl+Alt+drag** or middle drag | **Alt+drag**, wheel |

In the Industry preset only, a middle drag with keys selected moves them.

### Pins in the graph

A [[Hold and bind|pin]] shows as a band over the frames it covers. Drag its start or its end to change when the pin starts (**Move Pin Start**) or is released (**Move Pin Release**). Click a band to select it and press **Delete** to delete the pin.

## Tips and tricks

- Block out a performance with **Stepped** keys, then select them all and press **Auto** once the timing is right.
- Use **Plateau** on keys where a limb must stop without drifting past its pose.
- A bone that suddenly spins between two keys usually has a 360° jump: run **Euler Filter**.
- **All animated bones** with **Frame All** is a quick way to see the timing of the whole animation.

## Troubleshooting

### The graph is empty

No bone is selected, or the selected bones have no keys. Select a keyed bone, or switch the drop-down to **All animated bones**.

### A curve overshoots between two keys

**Spline** and **Auto** tangents can swing past the key values on uneven spacing. Select the keys and press **Plateau** or **Linear**.

### The middle mouse button pans instead of moving keys

In every preset except Industry, the middle button is used for panning. Drag the keys with the left button instead.

## See also

- [[Keys and timeline]]
- [[Keyboard shortcuts]]
- [[VATs Editor (viewer)]]

Category: Animating
