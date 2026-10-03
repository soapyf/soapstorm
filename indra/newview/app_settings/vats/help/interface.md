# Interface

The VATs window is a menu bar, seven docked panels and a status bar. Panels can be dragged by their
tabs to other places, stacked as tabs, or pulled out as floating windows; VATs remembers the layout
between sessions. **Workspaces** (a trial) show only the panels one job needs, and the **Tab pie** puts the
common tools at the pointer.

> Related articles: [[First steps]], [[Control presets]], [[Keyboard shortcuts]], [[Preferences]]

![The VATs window with the default layout: Bones on the left, Viewport in the centre, Properties on the right, Graph and Timeline below, the status bar along the bottom](images/interface/window.png)
*The default layout, with the [[First steps]] example open at frame 10 and **mElbowRight** selected.*

## Layout

| Panel | Default place | Holds |
|---|---|---|
| **Bones** | left | the skeleton as a list, with a filter box and selection buttons |
| **Picker** | left, a tab beside **Bones** | joint dots and bone lines over the avatar or its silhouette, on four pages (**Body**, **Hands**, **Face**, **Extras**); labels that select whole groups; selection sets folded under it; see [[Picker]] |
| **Inventory** | left, a tab beside **Bones** | projects, animations, mesh bodies, props, poses and clips |
| **Viewport** | centre | the avatar, the gizmo, the view cube |
| **Properties** | right | the selected bone or prop, the animation settings, the export settings |
| **Graph** | bottom | the curve editor |
| **Dope Sheet** | bottom, a tab beside **Graph** | the keyed frames per body part and bone |
| **Timeline** | bottom, under **Graph** | play controls, the frame box, tool buttons and the frame ruler |

The **status bar** runs along the bottom of the window. On the left it shows what the last command
did, how many items are selected when there is more than one, **Ortho** while the view is
orthographic, **Target:** and the target's name while a [[Target ghost]] is loaded (with how far the selected bone
is from it), and **Check: N** when the [[Animation check]] has found problems. On the right it shows the mouse
controls of the active [[Control presets|control preset]], or the graph's or dope sheet's controls while
the pointer is over the **Graph** or **Dope Sheet** panel. A long message hides those controls until the next one;
one too long for the bar ends in `...`: hover it for the whole text.

The title bar shows the project's file name (`Untitled` before the first save), `*` while there are
unsaved changes, and the VATs version.

## Usage

### Bones

- **Filter bones...** narrows the list to bones whose names, SL's or the plain one, contain the text. The best
  match is outlined and scrolled to; **Enter** selects it. The **×** at the end of the box clears it, as in every
  filter and search box.
- **Select All** selects every visible bone; **Keyed on Frame** selects the bones with a key on the
  current frame; **All Keyed** selects every bone with a key anywhere in the animation.
- **Show** (closed by default) picks which groups of bones are listed and drawn, and has
  **Collision Volumes**. The same switches are in **View → Bones**.

A bone with a key on the current frame is listed in amber, a bone animated anywhere in tan, and
attachment points in green. Beside each SL name the bone's plain name is shown dimmed (**mHipLeft** Left Thigh),
and the status bar says both (`mHipLeft · Left Thigh`); **View → Bones → Plain Names** turns them off. See
[[Skeleton]].

### Inventory

**Filter by name...** at the top narrows every section to the items whose names contain the text, and leaves out
the sections with no match. Then:

- **Projects** and **Animations**: your `.vat` and `.anim` files, from the library folders, recent
  projects and folders you add ([[Project library]]).
- **Bodies**: your [[Mesh bodies]].
- **Poses** (your poses and clips) and **Starter poses**: the [[Pose library]].
- **Meshes** and **Starter props**: the prop library ([[Props]]).

Click a section's title to fold it away; VATs remembers which sections you folded, next time too.

Drag or double-click an item to use it; right-click it for the rest.

### Viewport

- Click a bone to select it. Clicking the same spot again selects the bone underneath.
- **Shift+click** adds a bone to the selection; a click on empty space clears it (see [[Posing]]).
- Right-click a bone for its body-part menu.
- **Esc** or a right-click during a drag cancels the drag.
- The cube at the top left turns the view: click a face to look from that side, or drag the cube to
  orbit.
- The axis marker at the bottom left shows the world axes.
- **View → Camera → Orthographic** (**Num 5** in every [[Control presets|control preset]]) switches between
  perspective and an orthographic view, and back. See [[Interface#Orthographic view]].

### The Tab pie

Hold **Tab** with the pointer over the viewport: a ring of eight tools opens around the pointer. Move toward
one and let go of **Tab** to run it. With practice it is one flick: **Tab**, a short move, release, and the
ring hardly shows.

| Direction | Main ring | **More** ring |
|---|---|---|
| Up | **Set Key** | **Frame** (Frame Selected) |
| Up right | **IK / FK** | **Relax** |
| Right | **Rotate** | **Reset Bone** |
| Down right | **Limits** (Respect Joint Limits, on / off) | **Edit Limits** |
| Down | **More** | **Back** |
| Down left | **Auto IK** (on / off) | **Scale** |
| Left | **Move** | **Mirror** (Mirror Bone to Other Side) |
| Up left | **Select** | **Tween** |

- **Tap** **Tab** (press and let go without moving) and the pie stays open: click a slot. **Esc**, a
  right-click, a click in the middle or **Tab** again closes it.
- With the pointer on a bone, the pie works on that bone: the bone's name shows under the centre, and the slot
  you pick selects it first. Opened over empty space, it works on the selection.
- A slot that needs a selection is greyed while nothing is selected; point at it and the reason shows
  under the centre.
- Each slot does what its button or menu command does, with the same undo step. The key under a slot's
  name is that command's key in your [[Control presets|preset]].
- A toggle that is on, and the current tool, have their icon in the accent colour.
- **More** opens the second ring around the pointer; **Back** returns. Down then down again goes there and back.
- Near the window's edge the ring moves inward to stay whole; a flick still counts from where you pressed.
- The ring fades in, or appears at once with **Preferences → Camera → Reduce motion**. It scales with
  **Interface size**.
- In a text field **Tab** moves to the next field as usual, and over a panel it is the panel's: the pie
  opens only over the viewport. **Find a Tool** (**F3**) finds it as **Tool Pie**, and opens it in the
  middle of the viewport.
- **Edit → Keyboard Shortcuts...** can give it another key (**Tools → Tool Pie**).

### Worked example: key the elbow with the pie

[Open the example](example:first-wave.vat), then:

1. Point at the right elbow (the raised arm) and hold **Tab**. The ring opens with `mElbowRight` under the
   centre.
2. Move right, onto **Rotate**, and let go. The elbow is selected, with the rotate gizmo.
3. Drag the blue ring a little, then **Tab**, flick up to **Set Key**, let go. The status bar says
   `Keyed 1 item at frame 0`.

### Workspaces

**Workspaces** is a trial: tabs in the menu bar after the menus, each a layout for one job. Each tab has an
icon and a colour from the Commodore 64 badge stripes: **Pose** blue, **Animate** green, **Face** yellow, **Rig**
orange, **Export** red. The one you are in shows its icon in that colour over a short bar. In a narrow window the
other tabs show their icons only (hover one for its name), and narrower still they fold into one menu.

| Workspace | Panels | Tool buttons |
|---|---|---|
| **Pose** | **Bones**, **Picker** and **Inventory** (its poses) on the left, **Properties** on the right, a short **Timeline** | the four tools but Scale, the axes, **IK / FK**, **Auto IK**, **Limits**, **Mirror**, **Set Key** |
| **Animate** | **Bones** and **Picker**, **Properties**, **Graph** and **Dope Sheet** below, the **Timeline** | the four tools but Scale, the axes, **IK / FK**, **Auto IK**, **Retime**, **Set Key**, **Blocking**, **Tween** |
| **Face** | **Face** and **Picker** on the left, **Motion Capture** on the right, the **Timeline**; the camera looks at the face, the Picker shows its **Face** page | **Select**, **Move**, **Rotate**, **Set Key** |
| **Rig** | **Bones**, the rigging tools as tabs on the right (**Map Rig**, **Rig** for **Rig a Model from Scratch**, **Paint** for **Paint Weights**, **Limits** for **Suggest Joint Limits**; a tab's tooltip gives its full name) with **Properties** under them; **Rig → Joint Offset Inspector...** opens the inspector | no Timeline |
| **Export** | the export settings (**Export**), **Animation Check** under them, the **Timeline** | play controls only |
| **All** | every panel, as before workspaces | every button |

- A click on a tab switches at once. The project, the selection, the frame and the camera stay as they were;
  only **Face** moves the camera to the face, and leaving it puts the camera back.
- Each workspace remembers its own arrangement: move or undock a panel in **Pose** and **Animate** is not
  touched. **View → Reset Layout** puts the workspace you are in back as it started.
- **Ctrl+Space** (**View → Maximise Panel**) with the pointer over a panel makes that panel fill the window, such as
  the graph while you polish curves; **Ctrl+Space** again puts every panel back where it was.
- Nothing is hidden for good. Every command is in the menus and in **Find a Tool** (**F3**) in every
  workspace, and tool windows open as usual. A panel a workspace leaves out comes in when you ask for it:
  **Ctrl+G** brings the graph, and **View → Workspaces → Show in this workspace** lists the others. It stays
  until **Reset Layout**.
- The tool buttons keep one order in every workspace; a workspace only leaves some out.
- **View → Workspaces** lists them too, with **Workspace Tabs (Trial)** to turn them off: off, VATs is the
  full layout (**All**) with no tabs. **Preferences → Layout → Workspaces (trial)** is the same switch.
- A new installation starts in **Pose**. An installation from before 0.2.0 gets the tabs too, on **All**, so its
  layout stays as it was.
- **Next Workspace** and **Previous Workspace** have no keys; give them some in **Edit → Keyboard
  Shortcuts...** (Blender's **Ctrl+Page Up** / **Ctrl+Page Down** switch the tabs of a docked panel here).

### Orthographic view

An orthographic view has no perspective: parts of the body the same size look the same size however
far from the camera they are, and parallel lines stay parallel. Use it to check symmetry, a pose's
silhouette, or where a hand is against the body, from **Front**, **Right** or **Top**.

![The waving avatar from the front in an orthographic view, with no floor grid](images/interface/orthographic.png)
*The [[First steps]] example at frame 10, **Front** and orthographic.*

- **View → Camera → Orthographic** or **Num 5** turns it on; the menu item is ticked and the status bar shows
  **Ortho**. The same key or item turns it off. The camera keeps its place and its direction.
- At the orbit target the view is as tall as the perspective view, so turning it on keeps the framing.
  Zoom (the wheel, drag zoom, **Zoom In** / **Zoom Out**) makes the view taller or shorter; **Frame
  Selected** and **Frame All** fit it as they fit the perspective view.
- With the view orthographic, the view cube's faces and **Top** look exactly along the axis: **Top**
  looks straight down (a plan), not from 86° as in perspective. Orbiting from there tilts back to 86°.
- Clicks, the gizmo, bone markers and onion-skin ghosts use the same projection, so what you click is
  what you see.
- The floor grid is edge-on in **Front**, **Back**, **Left** and **Right**, so it does not show there.
- **Reset Camera** keeps the view orthographic.

> **Note:** The orthographic view is not yet available in the [[VATs Editor (viewer)|viewer]]; there
> **Num 5** says `This view has no orthographic mode yet`.


Sections, each of which can be collapsed. A workspace shows the ones its job needs: **Rig** shows **Bone** and
**Joint Limits**, **Pose** adds **Animation** folded (it opens when nothing is selected), **Animate** adds it open,
and only **All** has **Export** (the **Export** workspace has its own **Export** panel).

- **Bone** (or **Prop** when a prop is selected): the selection's values and options, and its **Bone
  priority**, which overrides the clip's **Priority** for that bone alone.
- **Joint Limits**: the selected joint's limits. See [[Joint limits]].
- **Animation**: **Frame rate**, **Last frame**, **Loop** with **Loop in** and **Loop out** beside it, **Priority**,
  **Ease in** and **Ease out**, **Hand pose** and **Expression**. See [[Keys and timeline]],
  [[Animation priority]] and [[Loop tools]].
- **Export** (All only): file naming, folder, bake shape and export buttons. See [[Export to Second Life]].

### Graph

The curve editor for the selected bones. Close it with the **×** on its tab; **Ctrl+G** (**View →
Graph Editor**) shows or hides it. See [[Graph editor]].

### Dope Sheet

The keys of the selected bones as diamonds, one row per body part, with a summary row on top; it shares
its key selection, copied keys and time range with the graph. Close it with the **×** on its tab;
**View → Dope Sheet** shows or hides it. See [[Dope sheet]].

### Timeline

From left to right: the play controls, the frame box and the last frame, the tool buttons, the axes
button, **IK / FK**, **Mirror**, **Retime**, **Set Key** and the **Tween** slider. Below them is the frame ruler
with the keys, the loop and ease markers, any retime markers and, when loaded, the [[Audio track]]. See
[[Keys and timeline]].

![The Timeline panel: play controls, Frame 10 of 30, the tool buttons, and the ruler with keys at 0, 10, 20 and 30 inside a tinted loop band](images/interface/timeline.png)
*The timeline of the [[First steps]] example: amber diamonds are keys, the small triangles at 6 and
24 the ease markers, and the band from 0 to 30 the loop.*

The play controls are icons only; hover one for its name and key:

| Button | Icon |
|---|---|
| **Go to start** | a bar, then a triangle pointing left |
| **Previous key** | two triangles pointing left |
| **Play / pause** | a triangle pointing right; two bars while playing |
| **Next key** | two triangles pointing right |
| **Go to end** | a triangle pointing right, then a bar |
| **Loop** | two arrows chasing each other; highlighted while **Loop** is on |

The other buttons show an icon and their name: **Select** (an arrow pointer), **Move** (four arrows),
**Rotate** (a circling arrow), **Scale** (a corner with a dot), the axes button (**Local** with a box,
**World** with a globe, **Gimbal** with three axes), **IK / FK** (a bone), **Mirror** (two halves either side of
a dashed line), **Retime** (a stopwatch) and **Set Key** (a diamond with a plus), with **Previous Key** and
**Next Key** arrows either side of it. Their keys are in the tooltips. Hold **Right Alt** to see every button's key
on a small badge; Left Alt stays the camera.
The active tool is highlighted. The **Tween** slider shows a bar between two boxes, or a curve through two points
while **Relax** is on; **Blend** shows two overlapping circles. When the **Timeline** panel is too narrow for
the names (a window about 1200 pixels wide), these buttons and **Relax** show their icons only, so the **Tween**
slider and **Blend** stay in view.

### Menus

Every menu opens with a **Search** box at its top, ready for typing. Type part of any command's or tool window's
name, from any menu and any of its submenus: the menu shows the matches instead, each with the menu it is in
(`seam` finds **Make Loop Seamless**, `Tools > Loop Tools`). Pick one with the arrow keys and **Enter**, or click it.
Greyed entries can't run right now; hover one to see why. **F3** opens the same search as its own window
(**Find a Tool**).

| Menu | Holds |
|---|---|
| **File** | **New**, **Open...**, **Open Recent**, **Save**, **Save As...**, the imports (BVH, SL `.anim`, retarget, prop / mesh, audio), the exports (`.anim`, BVH, **Export Listing Media...**: see [[Listing media]], **Export Rigged Mesh for SL...**: see [[Rigging for SL without add-ons]]), **Quit** |
| **Edit** | **Undo**, **Redo**, **Undo History...** (every step by name: click one to go back or forward to it), keys, resets, copy and paste pose, **Save Clip of Selected Bones...**, **Time**, mirror and flip, **Reverse Animation**, **Flip Animation** (left and right swapped on every key), **Simplify Curves...**, **Preferences...** |
| **Playback** | play, frame and key stepping, start and end |
| **View** | **Camera** (view directions, **Orthographic**, framing and zoom, **Reset Camera**, **Camera Views**), **Graph Editor**, **Dope Sheet**, **Maximise Panel**, **Reset Layout**, **Bones** (the bone group switches, **Show Collision Volumes**, **Collision Volumes in Front (X-ray)**, **Show Weights of Selected**, **Hide Unused Bones**: with a mesh body, only the bones it is weighted to; see [[Skeleton]]; **Plain Names**: the plain name beside each SL name), **Centre of Mass**, **Onion Skin**, **Target Ghost**, **Motion Path**, **Treadmill**, **Reference...** (a picture behind the avatar: [[Reference images]]), **Preview as SL Plays It**, **Face Cam**, **Body** (the body shown, and its **Parts** on and off) |
| **Light** | the lighting presets **Flat Noon**, **Three-Quarter Key**, **Rim / Back**, **Dusk** and **Night**, **Studio (Default)**, and **Plain Backdrop**: a grey wall and floor behind the actor that turn with the camera. They are for looking at the animation only; nothing is saved |
| **Select** | **Select All**, **Select Keyed on Frame**, **Select All Keyed**, **Select None**, parent, child and siblings |
| **Tools** | the four tools, the axes, IK and pins, **Clean Up Foot Sliding...**, **Loop Tools**, **Hand Poser**, **Dynamics...**, **Idle Layer...**, **Overlap...**, **Auto-Balance...**, **Jump Arc...**, **Ragdoll...**, **Face...**, **Actors (Couples and Groups)...**, **Motion Capture...**, **Split Dance at Beats...**, **Animation Check...**, **Motion Quality...**, **Priority Planner...** |
| **Rig** | **Map Rig to Second Life...** (see [[Rig any model]]), **Rig a Model from Scratch...**, **Paint Weights...** (see [[Rig a model from scratch]]), **Edit Limits**, **Suggest Joint Limits...** (see [[Joint limits]]), **Joint Offset Inspector...** |
| **Help** | **Help Contents**, **Tutorials**, **Controls**, **Welcome**, **About Viewport Avatar Toolset** |

Choosing a command in a menu closes it, sub-menus and all; so does **Esc** or a click anywhere outside it. Settings
inside a menu, such as the **Onion Skin** sliders or **Treadmill → Custom speed**, keep it open while you change them.

Every slider in VATs drags from where its value is: press anywhere on it and move the mouse, and a click alone
never changes it. Double-click it (or **Ctrl+click**) to type a value, then **Enter**; hold **Shift** while
dragging to go faster and **Alt** to go slower. A slider's tooltip waits until you let go.

Every **Tools** item has an icon, the same as its button where it has one:

| Tools item | Icon |
|---|---|
| **Select Tool**, **Move Tool**, **Rotate Tool**, **Scale Tool** | the tool buttons' arrow pointer, four arrows, circling arrow and corner |
| **Cycle Local / World / Gimbal Axes** | three axes |
| **Switch IK / FK** | a bone |
| **Follow Target (Bake)...** | a crosshair in a circle |
| **Hold in World from Here**, **Bind to...** and **Bind to Selected Bone from Here**, **Release from Here**, **Delete Pin** | a pin, a chain link, a crossed-out pin, a bin |
| **Sit on Seat** | an armchair |
| **Clean Up Foot Sliding...** | an anchor |
| **Loop Tools** | two arrows chasing each other, as **Loop**; inside it **Make Loop Seamless** (an infinity sign), **Remove Hip Travel (In Place)** (an arrow down to a dot), **Start Cycle at Frame** (a turning arrow), **Find Best Loop Points...** (a magnifier), **Fit Loop to Beats...** (a two-way arrow), **Loop-Aware Tangents** (a curve through two points) |
| **Hand Poser** | a hand |
| **Dynamics...** | an atom |
| **Idle Layer...**, **Overlap...**, **Auto-Balance...**, **Jump Arc...** | wind, waves, a balance scale, a rabbit |
| **Make Transition...** | boxes with an arrow at the end, beside **Tween**'s at the start |
| **Ragdoll...** | a line falling to the right |
| **Face...**, **Actors (Couples and Groups)...**, **Clips (AO Sets)...** | a smile, two people, a clapperboard |
| **Motion Capture...** | a dot in a circle, as its **Record** button |
| **Split Dance at Beats...** | scissors |
| **Animation Check...**, **Motion Quality...**, **Priority Planner...** | a shield with a tick, a gauge, a numbered list |

![The waving avatar lit by Light → Dusk, a low warm light, in front of the grey Plain Backdrop wall and floor](images/interface/light-dusk.png)
*The **Dusk** preset with **Plain Backdrop** on.*

A menu item that cannot be used now is greyed; hover over it to see why. The key shown beside an item
is the key in the active preset.

### Help windows

- **Help → Help Contents** (**F1**) opens this help: contents, search and every page.
- **Help → Tutorials** opens the [[Tutorials]] page: the lessons in order, beginner first.
- **Help → Controls** lists the mouse controls and every key of the active preset.
- **Help → Welcome** reopens the start window: what is new, and **First Steps** and **Tutorials** buttons that open those help pages.
- **Help → About Viewport Avatar Toolset** shows the version, licence and credits.

The **Help** window opens floating; **Esc** closes it while it has the keyboard, and every other shortcut still
works with it open. To keep it beside your work, drag its title bar onto a panel,
such as **Properties**, and drop it on the centre of the docking target: it becomes a tab there and
stays there the next time you open it. Drag the tab out again to float it. While it is docked, **F1**
or **Help → Help Contents** brings its tab to the front. In a narrow panel the contents list hides
behind a **Pages** button, which shows search and the page list in place of the page; picking a page
or pressing **Page** goes back to the page.

### Worked example: find a key with the panels

[Open the example](example:first-wave.vat), the wave from [[First steps]], and use each panel once:

1. **Bones**: type `Elbow` into **Filter bones...**. The list shrinks to the two elbows and the bones
   above them, both elbows in amber because they have a key on frame 0; click **mElbowRight**.
2. **Viewport**: the rotation gizmo appears around the elbow. Press **F** to frame it.
3. **Properties → Bone**: **Rotation** reads `0.0°  9.0°  91.0°` and **Keyed at this frame**.
4. **Timeline**: press **.** (**Next key**). The frame box reads **Frame 10**, the playhead sits on
   the second diamond, and **Rotation** now reads `0.0°  9.0°  60.0°`.
5. **Graph**: the blue **Rotate Z** curve dips to 60 under the playhead and rises to 110 at frame 20.
6. **Status bar**: the left end says `Opened first-wave.vat (an example: Save As to keep your changes)`
   until the next command; the right end shows the mouse controls of your preset.

## Configuration

- Colour theme, interface size and gizmo size: see [[Preferences]].
- **View → Body** picks the avatar drawn in the viewport: **SL Default**, **SL Default (Male)**,
  **Female**, **Male**, **Skeleton Only**, or a mesh body from your library.
- **View → Camera → Camera Views** recalls and stores four camera positions; see
  [[Keyboard shortcuts#Camera views]].

VATs keeps the panel layout in `layout.ini` in the data folder. See
[[Projects and files#Data folders]]. **View → Reset Layout** goes back to the default layout, with
every panel docked where it starts on the first run. With workspaces on, `layout.ini` holds the workspace you
are in, and `settings.json` the others (`workspace_layouts`) and the panels you brought into each
(`workspace_extra`); `workspaces` and `workspace` say whether the tabs are on and which is open.

## Troubleshooting

### A panel is missing

With workspaces on, the workspace may leave it out: **View → Workspaces → Show in this workspace**, **F3**, or
the **All** tab.
Only **Graph** and **Dope Sheet** can be closed; **Ctrl+G** brings the graph back and **View → Dope
Sheet** the dope sheet. The other panels can be moved or undocked but
not closed, so a missing one is hidden behind another tab or pushed to a thin edge. **View → Reset
Layout** puts every panel back where it starts on the first run and shows the closed ones again.

### Text is too small or too large

Change **Interface size** in [[Preferences]]. VATs also follows the display scale set in the
operating system.

## See also

- [[Control presets]]
- [[Keyboard shortcuts]]
- [[Preferences]]

Category: Interface
