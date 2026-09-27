# VATs Editor (viewer)

The VATs Editor is VATs' own editor running inside the SoapStorm viewer. It draws the app's menus,
panels, tool windows and gizmos over the world and animates your own avatar in-world, so you pose, key and
play on the avatar you wear. It is the same code as the app, so both read and write the same `.vat`
projects and the other help pages apply as written, apart from the differences below.

> Related articles: [[VATs]], [[Interface]], [[Keys and timeline]], [[Export to Second Life]], [[Couples and groups]]

## Usage

### Opening and closing

Choose **Avatar → VATs Editor**. The editor's menus and panels fill the viewer's window; the middle shows
the world. Once you are logged in, most of the viewer's own UI is hidden (see below). The camera frames your
avatar from the front, as **Frame All** does; after that it stays where you put it. Close the editor with
**File → Close Editor** (**Ctrl+Q**) or **Viewer → Close Editor**. With unsaved changes it
asks **Save changes?** first; **Cancel** keeps it open. Quitting or logging out closes it without asking
and keeps the unsaved work as an autosave.

If the region stands your avatar up a second time while the editor is open (see below), the editor saves
your work and closes, with the notification "The region kept standing you up, so the editor closed. Your
work is saved and reopens next time." The next time you open the editor it reopens that work as it was:
the same project, every actor, its file and its unsaved changes, without the **Recover unsaved work**
window.

### The viewer's UI while the editor is open

The viewer's toolbars, navigation and status bars (with its menu bar), chiclets and windows are hidden, as
with the viewer's own hide-UI, and come back as they were when the editor closes. These stay, drawn over the
editor:

| Kept | Why |
|---|---|
| Questions that need an answer (the upload price, disconnect, quit) | you have to answer them |
| Notifications and toasts: script dialogs, teleport, friendship and permission offers, group notices | they need an answer or bring news |
| Conversations (nearby chat and IMs), also when torn off | chat |
| The Notifications window and the IM well | to read what came in |

Any other window that opens while the editor is open (for example a map a script opens) stays hidden until
you close the editor or choose **Viewer → Show Firestorm UI**.

- **Chat pane.** The editor's **Chat** pane holds the viewer's own Conversations window: nearby chat and
  your IMs, with its usual tabs and chat box. It moves and resizes with the pane, and hides when the pane is
  closed or another tab of its dock is in front. **Viewer → Chat** shows or hides it.
- **Viewer menu.** **Chat**; **Notifications** opens or closes the viewer's Notifications window (the menu
  and a button in the status bar show how many are unread); **Show Firestorm UI** shows all of the viewer's
  UI over the editor until you choose it again; **Close Editor**.

### Your avatar while the editor is open

The avatar belongs to the editor until you close it:

- **It sits down on the ground** as it opens, the same as **Avatar → Sit Down**, so it can't walk and
  isn't bumped around. It doesn't sit when it is already sitting on something, when you are flying, while
  you edit your appearance, or when RLVa forbids sitting.
- **Flying, it stays in the air.** The viewer's **Sit Down** is off while flying, and a sit in the air
  brings the avatar down to the ground, so the editor doesn't sit it: the avatar keeps flying and hovers
  where it is.
- **No movement reaches it.** Keys go to the editor, and movement from anywhere else (the movement
  buttons, a joystick, a walk to a spot) is dropped until you close the editor.
- **It stays where it was, on your screen.** Your avatar is drawn exactly where it was when the editor
  opened, even if the region pushes it, and the camera doesn't follow it, as after an **Alt**+click.
  Other residents may see it move. If the region moves a flying avatar more than 0.5 m, the viewer's
  autopilot flies it back, as it would to walk you to a spot.
- **Stand Up is refused** while the editor has sat the avatar down: the **Stand** button, **Stand Up**
  in the menus and pie menus, and the other ways the viewer offers show "Close the editor to stand up"
  instead. This includes RLVa's forced stand-up.
- **The region can still stand it up**, for example from a script. The editor sits it down again once;
  the second time it closes (see [[VATs Editor (viewer)#Opening and closing]]). A teleport doesn't count:
  the avatar sits down again where it lands.
- **Joint positions are reset** as the editor opens, on your screen only, as the viewer's **Reset
  skeleton** does: bones that stopped animations left out of place go back, and your mesh body's own joint
  offsets stay. Turn this off with **Edit → Preferences... → Reset joint positions when the editor
  opens**.
- **Your mesh keeps its shape.** Bone positions the editor sets (the hip, and bones your animation or the
  face tracking moves) are added to your avatar's own bone positions, including your mesh head's or body's
  joint offsets, so a mesh face isn't pulled to the default face. For a mesh head with its own face joint
  positions, turn off **Move face bones** in [[Face tracking]]. Closing the editor puts every bone back.
- **Only the editor's pose shows.** Every other animation on your avatar is stopped on your screen: your AO,
  animations from the region and from scripts, stands and walks, look-at, eye and head motion, breathing
  and expressions. Animations that start while the editor is open are stopped as they arrive. Nothing
  about this is sent to the region.
- **Closing gives everything back.** The built-in motions and every animation the region still plays on
  you start again, including the ones it started while the editor was open, and the avatar stands up if
  the editor sat it down and it is still sitting on the ground. The avatar is drawn where it really is
  again, Stand Up and walking work again, and world clicks are the viewer's.

Other residents see your avatar sitting on the ground with its normal animations; the editor's pose is
yours only until you upload the animation and play it.

### Clicks and keys

| Input | Goes to |
|---|---|
| A click, the wheel or typing in a viewer window that shows (the Chat pane, a notification, a question), even over an editor panel | the viewer, as usual |
| A click on an editor panel, menu or popup where no viewer window covers it | the editor |
| A click on the world | the editor: a bone selects it, a gizmo or IK handle drags, empty space clears the selection. Objects, click-to-walk and pie menus are off |
| **Alt**+drag, **Ctrl+Alt**+drag, **Ctrl+Alt+Shift**+drag | the viewer's camera |
| The mouse wheel over the world | the viewer's camera zoom |
| The viewer's camera keys: **Alt** (or **Ctrl+Alt**, **Ctrl+Alt+Shift**) with the arrows, **Page Up**, **Page Down**, **A**, **D**, **W**, **S**, **E** or **C** | the viewer's camera |
| Any other key | the editor's shortcuts (see [[Keyboard shortcuts]]) |
| Typing while a viewer text field has the focus | that field |

**Reset Hip Position** is the only editor shortcut that is also a camera key: **Alt+W** moves the camera
here, so use **Alt+H**, which works in every preset (the **Edit** menu still shows **Alt+W**). The
editor's keys also win over the viewer's own menu shortcuts, for example **Alt+H** (Teleport History),
**Alt+R** (Region Details) and **Shift+Alt+R** (Refresh Attachments), until you close the editor.

To chat, click the chat box in the **Chat** pane first; the keys then go there until you click the world or
an editor panel. The status bar says the same: "Clicks and keys: the editor   Camera: Alt+drag, wheel,
Alt+arrows   Chat: click the Chat pane".

### The world as the view

The editor has no 3D view of its own: the world is its view, and the viewer's camera is the camera. Bones
are drawn as coloured lines over your avatar, with attachment-point dots, [[IK]] handles, the gizmo, the
axis marker and the view cube. These and the editor's panels are drawn with the world, under the viewer's
windows that show: notifications, questions and the Chat pane are always in front. A viewer window over a
bone or a panel takes the click, not the editor.

- **Other actors** of a couple or group project (see [[Couples and groups]]) are drawn as skeletons at
  their place around your avatar, tinted with their colour. Click one of their bones to edit that actor;
  your avatar then shows it.
- **[[Onion skin]]** ghosts are bone lines, blue before the current frame and orange after.
- **Collision volumes** are three rings each, with **View → Show Collision Volumes** on.

The view cube, **Frame Selected** (**F**) and the camera views move the viewer's camera.

### Uploading

**File → Upload Animation...**, or **Upload Animation...** in the Export section, uploads what **Export SL
.anim** would write, with the project's export settings: every actor's animation, and the mirrored copy
too when **Also export the other side (mirrored)** is on. Each file is named by the export pattern and
gets the viewer's own price confirmation, one after another. **Cancel** skips that file and goes on to the
next. Each animation appears in your inventory when its upload finishes.

The editor refuses an upload before you log in, and one that breaks Second Life's limits (longer than 60
seconds, or 250,000 bytes or more), and says why.

> **Warning:** Uploading costs L$ for each animation and cannot be undone.

### Colours

The editor takes its colours from the viewer's skin and follows it when the skin's colours change.
**Edit → Preferences...** shows "The viewer's skin" instead of the colour theme list.

## App and viewer

| Feature | App | Viewer |
|---|---|---|
| Body | VATs' avatar, or a [[Mesh bodies|devkit mesh body]] | your own avatar and the mesh body you wear |
| [[Props]] | imported `.dae` props | none: in-world objects and worn attachments instead |
| 3D view | its own, with its own camera controls per preset | the world, with the viewer's camera controls |
| Other actors | each with its own body | skeletons |
| Pose and prop thumbnails | pictures | plain icons |
| [[Audio track]] | the app's audio output | the viewer's sound, heard by you only; no snippets while scrubbing |
| File types | registered with the desktop | not registered |
| Upload | with any viewer's upload window | directly, from the editor |
| Colours | **Dusk** or **Studio Grey** | the viewer's skin |

Files open and save through the viewer's file picker. A cancelled folder choice leaves the setting as it
was.

Poses made with the Firestorm Poser can't be keyed into the editor yet; pose with the editor's gizmos.

## Troubleshooting

### Other people don't see the animation

The editor's pose is shown on your screen only. Upload the animation and play it from your inventory.

### My avatar didn't sit down

It doesn't sit while you fly, while it already sits, while you edit your appearance, or when RLVa forbids
sitting. Flying, it hovers where it was instead. It still shows only the editor's pose.

### Stand doesn't work

While the editor has sat your avatar down, Stand Up is refused and a notification says "Close the editor
to stand up". Close the editor; the avatar stands up.

### The editor closed by itself

The region stood your avatar up twice while the editor was open. Your work is saved; open the editor
again to get it back as it was.

### Clicking an object does nothing

World clicks go to the editor while it is open. Close the editor to touch, sit on or edit objects.

### My keys don't reach chat

Keys go to the editor unless a viewer text field has the focus. Click the chat box in the **Chat** pane,
then type.

### A viewer window I need is hidden

Choose **Viewer → Show Firestorm UI**: all of the viewer's UI shows over the editor until you choose it
again.

## See also

- [[Interface]]
- [[Export to Second Life]]
- [[Project file format]]

Category: Viewer
