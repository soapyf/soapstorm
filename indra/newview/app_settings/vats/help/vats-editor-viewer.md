# VATs Editor (viewer)

The VATs Editor is VATs' own editor running inside the SoapStorm viewer. It draws the app's menus,
panels, tool windows and gizmos over the world and animates your own avatar in-world, so you pose, key and
play on the avatar you wear. It is the same code as the app, so both read and write the same `.vat`
projects and the other help pages apply as written, apart from the differences below.

> Related articles: [[VATs]], [[Interface]], [[Keys and timeline]], [[Export to Second Life]], [[Couples and groups]]

## Usage

### Opening and closing

Choose **Avatar → VATs Editor**. The editor's menus and panels fill the world view below the viewer's
own menu and navigation bars, which keep working; the middle shows the world. Choose **Avatar → VATs
Editor** again to close it. With unsaved changes it asks **Save changes?** first; **Cancel** keeps it
open. Quitting or logging out closes it without asking and keeps the unsaved work as an autosave.

### Your avatar while the editor is open

The avatar belongs to the editor until you close it:

- **It sits down on the ground** as it opens, the same as **Avatar → Sit Down**, so it can't walk and
  isn't bumped around. It doesn't sit when it is already sitting on something, when you are flying, while
  you edit your appearance, or when RLVa forbids sitting.
- **Only the editor's pose shows.** Every other animation on your avatar is stopped on your screen: your AO,
  animations from the region and from scripts, stands and walks, look-at, eye and head motion, breathing
  and expressions. Animations that start while the editor is open are stopped as they arrive. Nothing
  about this is sent to the region.
- **Closing gives everything back.** The built-in motions and every animation the region still plays on
  you start again, including the ones it started while the editor was open, and the avatar stands up if
  the editor sat it down and it is still sitting on the ground.

Other residents see your avatar sitting on the ground with its normal animations; the editor's pose is
yours only until you upload the animation and play it.

### Clicks and keys

| Input | Goes to |
|---|---|
| A click on an editor panel, menu or popup | the editor |
| A click on a viewer window (chat, inventory, notifications, the toolbars) | the viewer, as usual |
| A click on the world | the editor: a bone selects it, a gizmo or IK handle drags, empty space clears the selection. Objects, click-to-walk and pie menus are off |
| **Alt**+drag, **Ctrl+Alt**+drag, **Ctrl+Alt+Shift**+drag | the viewer's camera |
| The mouse wheel over the world | the viewer's camera zoom |
| The viewer's camera keys: **Alt** (or **Ctrl+Alt**, **Ctrl+Alt+Shift**) with the arrows, **Page Up**, **Page Down**, **A**, **D**, **W**, **S**, **E** or **C** | the viewer's camera |
| Any other key | the editor's shortcuts (see [[Keyboard shortcuts]]); an editor shortcut that is also a camera key, such as **Alt+W** (**Reset Hip Position**), works from the **Edit** menu only |
| Typing while a viewer text field has the focus | that field |

To chat, click the chat bar first; the keys then go there until you click the world or an editor panel.
The status bar says the same: "Clicks and keys: the editor   Camera: Alt+drag, wheel, Alt+arrows   Chat:
click the chat bar".

### The world as the view

The editor has no 3D view of its own: the world is its view, and the viewer's camera is the camera. Bones
are drawn as coloured lines over your avatar, with attachment-point dots, [[IK]] handles, the gizmo, the
axis marker and the view cube. These are drawn with the world, under the viewer's own windows; the
editor's panels stay on top of everything. A viewer window over a bone takes the click, not the bone.

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
sitting. Land and reopen the editor. It still shows only the editor's pose, but can be moved.

### Clicking an object does nothing

World clicks go to the editor while it is open. Close the editor to touch, sit on or edit objects.

### My keys don't reach chat

Keys go to the editor unless a viewer text field has the focus. Click the chat bar, then type.

## See also

- [[Interface]]
- [[Export to Second Life]]
- [[Project file format]]

Category: Viewer
