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

> **Note:** Screenshot to come: the editor just opened in-world, with your avatar sat on the ground, its
> bones drawn over it, and the editor's panels around the world view.

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

Where they go while the viewer's UI is hidden:

| What | Where |
|---|---|
| Notifications, IM and group notice toasts | stacked from the top right of the editor's view: the world area left between its docked panels |
| Script dialogs that are not docked | the top right of the editor's view |
| Questions that need an answer | the centre of the editor's view |
| Nearby chat toasts | the bottom left of the editor's view |
| The **Stand** button, while your avatar sits | the bottom of the world view; the editor's panels and status bar stay above it |

They follow the view when you move or resize the editor's panels. With **Show Firestorm UI** on, or after
you close the editor, they are back in the viewer's own places. The view cube is at the view's top left,
so the toasts never cover it.

Hovering an object or another avatar shows no hover tip (name, owner and so on) while the editor holds your
avatar, since the tip would be drawn over the editor's panels; world clicks are the editor's anyway. Tips
of the viewer's windows that stay, such as the Chat pane and the toasts, still show. Hover tips come back
with **Show Firestorm UI** and when you close the editor.

- **Chat pane.** The editor's **Chat** pane holds the viewer's own Conversations window: nearby chat and
  your IMs, with its usual tabs and chat box. It moves and resizes with the pane, and hides when the pane is
  closed or another tab of its dock is in front. **Viewer → Chat** shows or hides it.
- **Viewer menu.** **Chat**; **Notifications** opens or closes the viewer's Notifications window (the menu
  and a button in the status bar show how many are unread); **Show Firestorm UI** (**Alt+Shift+U**); **Close
  Editor**.
- **Show Firestorm UI** shows all of the viewer's UI over the editor: its menu bar, navigation bar, toolbars,
  chat bar and windows. The editor's menu bar, panels and status bar move in so the viewer's bars don't cover
  them. Put it away with **Viewer → Show Firestorm UI** again, with the viewer's own **Avatar → Show Firestorm
  UI over VATs Editor**, or with **Alt+Shift+U** from anywhere (the viewer's own Show User Interface keys,
  which do this while the editor is open). Closing the editor leaves the viewer's UI as it was before you
  opened it, whichever way it was showing.

> **Note:** Screenshot to come: **Show Firestorm UI** on, with the viewer's menu bar, toolbars and chat bar
> over the editor and the editor's panels moved in.

### Your avatar while the editor is open

The avatar belongs to the editor until you close it:

- **It sits down on the ground** as it opens, the same as **Avatar → Sit Down**, so it can't walk and
  isn't bumped around. It doesn't sit when it is already sitting on something, when you are flying, while
  you edit your appearance, or when RLVa forbids sitting.
- **Flying, it stays in the air.** The viewer's **Sit Down** is off while flying, and a sit in the air
  brings the avatar down to the ground, so the editor doesn't sit it: the avatar keeps flying and hovers
  where it is. The flying wind sound fades out while the editor is open; other sounds stay.
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
  joint offsets, so a mesh face isn't pulled to the default face. Uploads keep this with **Bake shape** at
  **Your avatar**, the default while you wear mesh joint positions (see [[Export to Second Life#Your avatar]]).
  Closing the editor puts every bone back.
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

Two commands change this while they are on, and give it back when they end: **View → As It Plays In-World**
(below) lets your AO and the other motions play, and **Tools → Loop Tools → Test as My Walk** lets you walk (see
[[Loop tools#Testing as your walk (viewer)]]). Sat on furniture, see [[Couples and groups#Animating on real furniture (viewer)]].

### As it plays in-world

**View → As It Plays In-World** shows your animation as it will play in-world, among everything else your avatar
plays:

- Your AO, the animations the region, scripts and gestures play on you, the viewer's own motions (head and eye
  motion, breathing, hands) and avatar physics run again, on your screen as usual.
- Your animation plays at its own priorities, and only on the joints its upload keys: its priority, or a joint's
  own, as **Export SL .anim** writes them. Each joint goes to the highest priority, on equal priority to the one
  started last, so your AO may win joints your animation keys at a lower priority.
- The avatar stays sat on the ground and drawn where it was, as before; nothing is sent to the region.
- The **As It Plays In-World** window lists every joint something animates: the motion that drives it and at what
  priority. Your animation is blue; a joint it keys but loses is orange, with the priority it keys it at in the
  tooltip. **Only the joints your animation keys** shortens the list. The list is your own avatar's only, names and
  priorities, as the [[Priority planner]]'s **Add Running Animations** reads them.

Choose it again, or close its window, and the editor holds your avatar alone again: every other motion stops and
the editor's pose shows on every joint. In the app, the [[Priority planner]] answers the same question from files.

### Other avatars while the editor is open

Every other avatar is hidden on your screen while the editor is open, friends too, with their attachments
and name tags, so the scene shows only you and your project. Your own avatar and animated objects stay.
This is the viewer's own **Render Only Friends**, extended to friends while the editor is open; closing the
editor sets it back to what it was. Nothing is sent: the others still see you, and you still get their chat.

To see them, choose **View → Show Other Avatars**, or tick **Edit → Preferences... → In the viewer → Show
other avatars**. The choice is kept for the next time.

The editor never picks, poses or reads other people's avatars. The other actors of a couple or group
project are your own project's actors (below).

### Clicks and keys

| Input | Goes to |
|---|---|
| A click, the wheel or typing in a viewer window that shows (the Chat pane, a notification, a question), even over an editor panel | the viewer, as usual |
| A click on an editor panel, menu or popup where no viewer window covers it | the editor |
| A click on the world | the editor: a bone selects it, a gizmo or IK handle drags, empty space clears the selection. Objects, click-to-walk and pie menus are off |
| **Alt**+drag, **Ctrl+Alt**+drag, **Ctrl+Alt+Shift**+drag | the viewer's camera |
| The mouse wheel over the world | the viewer's camera zoom |
| The viewer's camera keys: **Alt** (or **Ctrl+Alt**, **Ctrl+Alt+Shift**) with the arrows, **Page Up**, **Page Down**, **A**, **D**, **W**, **S**, **E** or **C** | the viewer's camera |
| **Alt+Shift+U** | **Show Firestorm UI**, on or off, whatever has the keys |
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

- **Other actors** of a couple or group project (see [[Couples and groups]]) stand at their place around
  your avatar with the **Body** chosen for them in the **Actors** window: **None** (the default) draws
  nothing, **Ruth** is the Second Life default body, drawn from the viewer's own avatar files, and a mesh
  body is one you imported under **Inventory → Bodies** from its files. Bodies are tinted with the actor's
  colour, lit simply, and hidden where the world is in front of them. Your avatar is the first actor in the
  **Actors** window, marked **(you)**.
- **Editing another actor.** Click its body, or its name in the **Actors** window (an actor with **None**
  by its name only). The bones, handles and gizmos are then drawn on that actor at its place, and its body
  shows its pose live, untinted: **Ruth** or a mesh body, or its bones alone with **None**. Your avatar keeps
  playing your own actor's animation, on the same timeline, so you see both together. The status bar says
  **Editing** and the actor's name. Click your avatar's bones, or your actor's name, to edit it again.
- **[[Props]]** are drawn with the world, lit simply and hidden where the world is in front of them: add,
  attach, place and move them as in the app. They are on your screen only; nothing is rezzed.
- **[[Onion skin]]** ghosts are bone lines, blue before the current frame and orange after.
- **Collision volumes** are three rings each, with **View → Show Collision Volumes** on.
- **Attachment points.** Hover a point's dot: the label lists what you wear there, by name. When the point
  is keyed, it also warns that in-world the animation moves what you wear there.
- **[[Reference images]]** in the scene stand in the world: your avatar and anything in front of the
  picture hide it. A backdrop picture is drawn over the world, see-through.

**View → Orthographic** (**Numpad 5**): the viewer can't draw a true orthographic view, so the camera
switches to its longest lens (5 degrees) and backs away until your avatar is framed as before; the draw
distance grows by as much, so the scenery behind stays. Zooming moves the camera in and out the same way.
Choose it again, or close the editor, for the viewer's own lens and camera. The long lens is close to
orthographic, not exact: parts of the body a metre nearer or farther than the focus look a few percent
larger or smaller.

The view cube (at the view's top left), **Frame Selected** (**F**) and the camera views move the viewer's
camera.

The **Actors** window is the app's, unchanged:

![The Actors window with two actors, Lead (you) and Partner, and Lead's settings below the list](images/vats-editor-viewer/actors-window.png)
*The first actor is your avatar. **Body** decides how the other actors are drawn in-world.*

> **Note:** Screenshot to come: the handshake example open in-world, with **Partner** drawn as Ruth in its
> colour in front of your avatar, and your avatar sat on the ground with Lead's bones drawn over it.

### Worked example: a partner in-world

[Open the example](example:couple-handshake.vat) while logged in: **Partner** stands 0.6 m in front of your
avatar, facing it, drawn as **Ruth** tinted with its colour, and by frame 15 both bring the right hand
forward. Click Partner's body: the status bar says **Editing Partner**, its bones are drawn on it and it
loses its tint, while your avatar keeps playing Lead's animation. Click your avatar's bones to edit Lead
again. Nothing is sent to the region; other residents see your avatar sitting on the ground as usual.

### Uploading

**File → Upload Animation...**, or **Upload Animation...** in the Export section, uploads what **Export SL
.anim** would write, with the project's export settings: every actor's animation, and the mirrored copy
too when **Also export the other side (mirrored)** is on. Each file is named by the export pattern and
gets the viewer's own price confirmation, one after another. **Cancel** skips that file and goes on to the
next. Each animation appears in your inventory when its upload finishes.

The editor refuses an upload before you log in, and one that breaks Second Life's limits (longer than 60
seconds, or 250,000 bytes or more), and says why.

The status bar shows the grid you are logged in to, and the Export section says what an upload costs there, as the
grid reports it. On the Aditi beta grid or an OpenSim grid it says uploads may be free there: a good place to try
an upload first.

> **Warning:** Uploading costs L$ for each animation and cannot be undone.

> **Note:** Screenshot to come: the viewer's upload price confirmation over the editor after **File →
> Upload Animation...**.

### Light

The **Light** menu lights the world for looking at the animation: **Flat Noon** (the sun straight above),
**Three-Quarter Key** (in front of your avatar, to its left and up), **Rim / Back** (behind it), **Dusk** (low and
warm, from its right) and **Night** (moonlight). Each is a sky on your screen only, like **World → Environment**'s
own choices: nobody else sees it and the region's sky is untouched. The light is placed from where your avatar
faces when you choose it. **The World's Own** puts back the sky you had, and so does closing the editor.

**Plain Backdrop** stands a grey wall behind your avatar, with a grey floor up to it, turning with the camera so
it stays behind. It hides the world behind it and is on your screen only.

### Help

**Help → Help Contents** opens these pages in the editor, with their pictures. **Open the example** buttons open
the example as **File → Open** would, asking to save your work first; **Save** asks for a new name, so the
example stays as it was.

### Colours

The editor takes its colours from the viewer's skin and follows it when the skin's colours change.
**Edit → Preferences...** shows "The viewer's skin" instead of the colour theme list.

## App and viewer

| Feature | App | Viewer |
|---|---|---|
| Body | VATs' avatar, or a [[Mesh bodies|devkit mesh body]] | your own avatar and the mesh body you wear |
| [[Props]] | imported `.dae` and `.fbx` props and the starter props | the same, on your screen only |
| 3D view | its own, with its own camera controls per preset | the world, with the viewer's camera controls |
| Other actors | **None**, **Ruth** or a mesh body each | the same, on your screen only; your avatar is the first actor and keeps playing it while you edit another |
| Other people's avatars | none | hidden while the editor is open, unless **View → Show Other Avatars** |
| Pose and prop thumbnails | pictures | plain icons |
| [[Audio track]] | the app's audio output | the viewer's sound, heard by you only; no snippets while scrubbing |
| File types | registered with the desktop | not registered |
| Upload | with any viewer's upload window | directly, from the editor |
| Colours | **Dusk** or **Studio Grey** | the viewer's skin |
| **Light** menu | lights the editor's own view | a sky on your screen only; the world's own comes back on close |
| [[Reference images]] | behind the avatar | the scene plane stands in the world, hidden by what is in front of it; the backdrop is over the world, see-through |
| Orthographic view | a true orthographic view | a 5 degree lens from far back: nearly orthographic |
| [[Priority planner]]: **Add Running Animations** | not shown | the animations playing on your avatar (AO, scripts, gestures), each joint's priority only |
| Attachment point labels | the point's name | also what you wear there, and a warning when the point is keyed |
| Grid and upload price | not shown | the grid in the status bar, the price in the Export section |
| [[Listing media]] | **File → Export Listing Media...** | not in the menu; the viewer's own snapshots |
| **View → As It Plays In-World** | not shown; the [[Priority planner]] with your files | your animation live among your AO and the rest, with who wins each joint |
| **Test as My Walk / Run** | not shown | walk for real with the animation as your walk, your speed against its stride |
| Furniture | the sit target typed in | your seat measured, pins and IK targets placed by clicking it |
| Face cam ([[Face tracking]]) | Ruth in SL's default shape | Ruth in your own shape; your in-world avatar is left alone |

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

Choose **Viewer → Show Firestorm UI** or press **Alt+Shift+U**: all of the viewer's UI shows over the
editor until you choose it again or press **Alt+Shift+U** again.

### Hovering an object shows no tip

While the editor holds your avatar, the viewer shows no hover tips for objects and avatars. Choose **Viewer
→ Show Firestorm UI**, or close the editor.

### I can't see other avatars

The editor hides them while it is open. Choose **View → Show Other Avatars**.

### An actor I added isn't drawn

Its **Body** is **None**, the default. Choose it by name in the **Actors** window and pick **Ruth** or a
mesh body under **Body**.

## See also

- [[Interface]]
- [[Export to Second Life]]
- [[Project file format]]

Category: Viewer
