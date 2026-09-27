# First steps

This page walks through a first session: starting VATs, posing the avatar on two frames, playing
the result, saving the project and exporting a `.anim` file for Second Life. It uses the default
Industry (Maya-style) controls; other presets change the keys, not the steps.

> Related articles: [[Installation]], [[Interface]], [[Keys and timeline]], [[Export to Second Life]]

## Usage

### Starting VATs

Run `bin/vats` (Linux) or `bin\vats.exe` (Windows). On the first start:

- If VATs finds data from Hexton SL Animator, the **Import from Hexton SL Animator?** window offers
  to bring it across. **Import** copies it; **Don't import** closes the window, and VATs does not
  ask again. See [[Projects and files#Importing from Hexton SL Animator]].
- The **Welcome to Viewport Avatar Toolset** window shows what is new. Clear **Show this at startup**
  to stop it opening; **Help → Welcome** brings it back.

A new document is an untitled animation at 30 frames per second, frames 0 to 30, with the avatar in
its default pose and the **Rotate** tool active.

### Choosing your controls

If you already know Maya, Blender, QAvimator or the Second Life build tools, pick the matching preset
in **Edit → Preferences... → Navigation & hotkeys** before you start. The status bar at the bottom
right always shows the mouse controls of the active preset, and **Help → Controls** opens the full list. See
[[Control presets]].

### Looking around

With the Industry preset:

- **Alt + left drag** orbits, **Alt + middle drag** pans, **Alt + right drag** or the wheel zooms.
- **F** frames the selected bone; **A** frames the whole avatar.
- Click a face of the cube at the top right of the viewport to look from that side.

### Posing the first frame

1. Click a bone in the viewport, for example the upper arm. It turns bright and the rotation gizmo
   appears. You can also pick bones in the **Bones** list on the left; type in **Filter bones...** to
   find one by name.
2. Drag a ring of the gizmo to rotate the bone. Hold **Ctrl** while dragging to snap to 5-degree
   steps.
3. Moving or rotating a bone sets a key for it on the current frame. A diamond appears in the
   timeline.

See [[Posing]] for the tools, and [[IK]] to move hands and feet by their ends.

### Posing another frame

1. Click frame 15 in the timeline ruler, or press **Right** until the frame box shows **Frame 15**.
2. Pose the avatar again. VATs fills in the frames between the two keys.
3. Press **S** to key the selected bones as they are, without changing them.

### Playing it back

Press **Space** to play and pause. **Home** and **End** jump to the start and end; **.** and **,**
jump to the next and previous key. To make the animation repeat in-world, tick **Loop** in
**Properties → Animation**. See [[Keys and timeline]] and [[Loop tools]].

### Undoing

**Ctrl+Z** undoes and **Ctrl+Y** redoes. Undo covers every edit to the animation, not camera moves or
selection.

### Saving the project

Press **Ctrl+S**. The first save asks for a file name; VATs adds `.vat` if you leave it out. The
title bar shows the file name, with `*` while there are unsaved changes. See [[Projects and files]].

### Exporting for Second Life

1. Press **Ctrl+E** (**File → Export SL .anim...**). The **Export SL .anim** window shows the length,
   priority, loop and ease settings, and the export settings.
2. Press **Export SL .anim**. The first time, a save dialog asks where to write the file; its folder
   becomes the export **Folder** for this project. After that, the button writes straight into the
   folder, naming the file from the **Name**, **Number**, **Side** and **Pattern** fields.
3. Upload the file in your viewer with **Build → Upload → Animation**.

See [[Export to Second Life]] for the settings, and [[Animation priority]] before you upload.

> **Warning:** Uploading costs L$ and cannot be undone. Check the file on the Aditi beta grid or in
> the viewer's preview first.

## Tips and tricks

- The status bar at the bottom left says what the last command did, for example
  `Keyed 1 item(s) at frame 15`.
- A greyed menu item says why it is unavailable when you hover over it, for example
  `Select a bone first`.
- Right-click a bone in the viewport for its body-part menu.

## See also

- [[Interface]]
- [[Keyboard shortcuts]]
- [[Posing]]
- [[Export to Second Life]]

Category: Getting started
