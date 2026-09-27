# Control presets

A control preset sets how the mouse moves the camera and which keys run which commands, so VATs
feels like a program you already know. There are four: **Industry (Maya-style)**, **Blender**,
**QAvimator** and **Second Life**. Industry is the default.

> Related articles: [[Keyboard shortcuts]], [[Preferences]], [[Interface]]

## Usage

### Choosing a preset

Open **Edit → Preferences...** (**Ctrl+,**) and pick one under **Navigation & hotkeys**. The change
applies at once and is saved. The status bar shows `Controls: <preset>`, and its right end shows the
mouse controls of the new preset. **Help → Controls** lists the keys of the active preset.

To try a preset for one session, start VATs with `--preset`; see [[Command line]].

### Mouse, per preset

| | Industry (Maya-style) | Blender | QAvimator | Second Life |
|---|---|---|---|---|
| Orbit | **Alt + left drag** | **Middle drag** | **Left drag** on empty space | **Ctrl + Alt + drag**; or **Alt + click**, then drag sideways |
| Pan | **Alt + middle drag** | **Shift + middle drag** | **Shift + left drag** on empty space; **middle drag** | **Ctrl + Alt + Shift + drag**; **middle drag** |
| Zoom | **Alt + right drag**; wheel | **Ctrl + middle drag**; wheel | **Alt + left drag** on empty space; wheel | **Alt + click**, then drag up or down; wheel |
| Focus | **F** (Frame Selected) | **Num .** | **F** | **Alt + click** on the avatar |
| Snap while dragging | hold **Ctrl** | hold **Ctrl** | hold **Ctrl** | **G** toggles snapping |

In every preset:

- Click a bone to select it; click the same spot again for the bone underneath.
- **Shift+click** adds to the selection, except on an FK bone in QAvimator, where **Shift** + drag turns
  the bone.
- Double-click a limb bone to switch its limb between IK and FK.
- Right-click a bone for its body-part menu.
- **Esc** or a right-click during a drag cancels it.
- The mouse wheel zooms.

### Industry (Maya-style)

The default. Camera moves need **Alt**; **Q**, **W**, **E** and **R** pick the Select, Move, Rotate
and Scale tools; **S** sets a key. **Alt+V** also plays and **Alt+.** and **Alt+,** also step frames.
**Alt+W** or **Alt+H** resets the hip position.

### Blender

- The camera uses the middle button. On a mouse or tablet without one, tick **Emulate 3-button mouse
  (Alt + left-drag = middle-drag)** in [[Preferences]]; **Alt + left drag** then orbits, with
  **Shift** to pan and **Ctrl** to zoom.
- **I** sets a key, **Alt+I** deletes it; **W**, **G**, **R** and **S** pick the Select, Move, Rotate and
  Scale tools; **A** selects all; the number pad sets views; **Alt+G** or **Alt+H** resets the hip
  position.

With the pointer over the viewport and a bone, IK control or static prop selected, **G** starts a
modal move and **R** a modal rotation that follow the mouse without a button held:

- **X**, **Y** or **Z** locks to that world axis; the same key again locks to the bone's own axis; a
  third time frees it.
- **R** again, during a rotation, rotates freely (trackball).
- Hold **Ctrl** to snap the rotation.
- A left click, **Enter** or **Space** confirms; a right-click, **Esc** or **Ctrl+Z** cancels.

The bottom left of the viewport shows the axis and the amount while the move runs. With the pointer
outside the viewport, **G** and **R** only pick the tool.

### QAvimator

- Drags on empty space move the camera; a click on empty space (without dragging) clears the
  selection.
- **Shift**, **Ctrl** or **Alt** + drag on a bone turns one rotation channel: Y, X or Z.
- **Ctrl+A** is **Save As...**, as in QAvimator, so **Select All** is **Shift+A**.
- **Ctrl+0** also frames everything, **Page Up** and **Page Down** zoom, **F9** to **F12** recall the
  four [[Keyboard shortcuts#Camera views|camera views]] and **Shift+F9** to **Shift+F12** store them.

### Second Life

For people used to the Second Life build tools. VATs starts in the **Move** tool; **Q**, **W**, **E**
and **R** pick the Select, Move, Rotate and Scale tools as in Industry, and **A** frames everything.

- Hold **Ctrl** to switch the gizmo to rotation, **Ctrl+Shift** to scale (static props only).
- **G** toggles snapping; the step is **Rotation snap (G)** in [[Preferences]].
- **Esc** resets the camera instead of clearing the selection.
- Keyboard camera, with the pointer anywhere: **Alt+Left** / **Alt+Right** orbit, **Alt+Up** /
  **Alt+Down** zoom, **Ctrl+Alt+Up** / **Ctrl+Alt+Down** orbit up and down, **Ctrl+Alt+Shift+arrows**
  pan.

### Graph editor navigation

| Industry (Maya-style) | Blender | QAvimator | Second Life |
|---|---|---|---|
| **Alt + left** or **Alt + middle drag** pans, **Alt + right drag** zooms, **middle drag** moves keys | **Middle drag** pans (or **Alt + left** with emulation), **Ctrl + middle drag** zooms | **Middle drag** pans | **Ctrl + Alt + drag** or **middle drag** pans, **Alt + drag** zooms |

The wheel zooms the graph in every preset. See [[Graph editor]].

## Configuration

The preset is stored as `preset` in `settings.json` (`industry`, `blender`, `qavimator` or
`secondlife`); see [[Preferences#Settings file]]. Keys cannot be rebound one by one.

## Troubleshooting

### A key does nothing

The key belongs to another preset, or a text field has the keyboard. Open **Help → Controls** to see the keys of
the active preset. While a text field is being edited, only **Ctrl** shortcuts for **New**,
**Open...**, **Save**, **Save As...**, **Export SL .anim...**, **Quit**, **Undo**, **Redo**,
**Preferences...** and **Graph Editor** reach VATs; click the viewport to give the keys back.

### Alt + drag moves the whole window

Some Linux desktops use **Alt + drag** (or **Super + drag**) to move windows. Change the window
manager's modifier, or use the Blender preset without emulation.

## See also

- [[Keyboard shortcuts]]
- [[Preferences]]
- [[Graph editor]]

Category: Interface
