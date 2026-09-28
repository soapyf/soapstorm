# Couples and groups

A couples or group animation is a set of animations, one per avatar, made to play together on one
piece of furniture or pose ball: a dance for two, a hug, a group photo. VATs keeps all the avatars
("actors") in one project on one timeline, so you develop their animations together with synced
playback. It places each one relative to the shared sit target and exports a file per actor plus a note
with the sit-target values.

> Related articles: [[Hold and bind]], [[Export to Second Life]], [[Props]], [[Mesh bodies]], [[Keys and timeline]]

> **Note:** In the viewer your own avatar is the first actor, and it plays that actor's animation whichever
> actor you edit. The other actors are drawn around it on your screen only, with the body you choose for
> them; they are never other people's avatars. See [[VATs Editor (viewer)#The world as the view]].

## Usage

### Add actors

Open **Tools → Actors (Couples and Groups)...**. A project starts with one actor. Add another with:

- **Add Partner**: a mirrored copy of the current actor's animation, 0.6 m in front of it and facing
  it;
- **Duplicate**: a copy of the current actor's animation, 0.8 m to its side;
- **Blank**: an actor with no keys, 0.8 m to the side;
- **Add Actor from File...**: an actor with the animation of a file, 0.8 m to the side (see
  [[#Load an animation into an actor]]).

Each new actor gets its own colour and the current actor's **Body**, so actors added to a plain project
start with **None** and draw nothing until you choose a body. Cross-actor binds are not copied.

### Your avatar

The first actor in the list is your avatar and shows **(you)** after its name. In the viewer it is the
avatar you wear, and it keeps playing its own animation while you edit another actor. It also decides
which actor may bake on the **Your avatar** bake shape (see [[#Export]]). To make another actor your
avatar, right-click its name and choose **Make This Your Avatar**: it moves to the top of the list. This
is one undo step.

### Choose the actor to edit

Click an actor's body in the view, or its name in the **Actors** window; an actor with the body **None**
draws nothing, so choose it by name. Everything then works on that actor: the Bones list, the bone overlay
and the gizmos, the timeline, the graph, **Properties**, keys, undo, the [[Pose library]] and the
[[Motion capture]] recording. The status bar says which actor it is, for example **Editing Partner**. The
other actors are shown dimmed in their colour. A locked actor cannot be chosen.

In the viewer, clicking your own avatar's bones goes back to editing your actor.

The three icon buttons at the end of each row act on the other actors; hover one for its name:

- **Place** (a map pin) shows a gizmo on that actor in the view. The Move tool moves it; the Rotate tool
  turns it about the vertical axis.
- **Shown**/**Hidden** (an open eye, or a crossed-out eye) shows whether the actor is drawn in the view;
  click it to hide or show the actor.
- **Unlocked**/**Locked** (an open or a closed padlock) shows whether the actor is locked; click it to
  lock or unlock. A locked actor cannot be chosen or placed by accident.

Right-click an actor's name for **Load Animation...**, **Save This Actor as Project...**, **Export This
Actor as .anim...**, **Make This Your Avatar** and **Delete Actor**. Deleting an actor removes binds from
other actors to it as well. With one actor left, the project is an ordinary single-avatar project.

### Load an animation into an actor

Each actor is one animation. To work on animations you already have in tandem, load one into each actor:

- right-click the actor's name and choose **Load Animation...**, or select the actor and press **Load
  Animation...** under its name;
- drag a `.anim` from the **Inventory**'s **Animations** section onto the actor's name, or onto its body in
  the view; a project from the **Projects** section can be dropped on the name too;
- **Add Actor from File...** makes a new actor with the file's animation.

The file can be a `.anim`, a [[BVH]] on the Second Life skeleton, or a project (`.vat`); a project with
several actors asks which one to load. Loading replaces only that actor's animation, as one undo step. When
the actor already has keys, VATs asks first.

The actor keeps its props, [[Audio track|audio track]], export settings and placement; only the
animation is replaced. **Add Actor from File...** with a project's actor also takes that actor's name,
colour, body and placement.

All actors share one timeline (below), so the loaded animation follows the scene's timing, as when you
insert a `.anim` from the Inventory:

| Differs | What happens |
|---|---|
| Frame rate | The animation is retimed to the scene's frame rate. Keys keep their timing, as **Keep Timing** does in the **Frame rate** field. |
| Longer | The scene grows to the animation's length, for every actor. |
| Shorter | The scene keeps its length; the animation holds its last pose after its last key. |
| Loop | The scene's **Loop** and loop points apply. |
| Binds | Binds to actors this project doesn't have are left out. |

The status bar says what changed, for example "Loaded Hug_B.anim into Partner; retimed from 24 to 30 fps,
keys keep their timing; the scene now lasts 90 frames (was 60)".

### Save or export one actor

Right-click the actor's name, or press **Save/Export This Actor...** under it:

- **Save This Actor as Project...** writes a project with that actor's animation, props and export
  settings alone;
- **Export This Actor as .anim...** writes its `.anim` with its own bake shape and settings, as
  **Export SL .anim** would write it.

### Set up the active actor

Below the list, under the actor's name:

| Field | Meaning |
|---|---|
| **Name** | Used in the exported file names. Press **Enter** to apply. |
| **Colour** | The tint when the actor is not being edited. |
| **Body** | How the actor looks: **None** (the default: nothing but its [[Props|props]], and its bones while you edit it), **Ruth** (the Second Life default body), or one of your [[Mesh bodies]]. Saved in the project. In the app the actor you edit shows **Ruth**, or the body chosen under **View** when it has **None** or a mesh body; in the viewer your avatar's actor is the avatar you wear. Projects made before may show another Linden body by name. |

Under **Placement from the sit target**, **Position (m)** and **Turn (deg)** set where the actor stands
relative to the shared point, which stands for the pose ball or the furniture's root in Second Life.

### Share the timeline

All actors share the frame rate, length, **Loop** and the loop points; changing them on one actor changes
them on all. Priority, ease, hand pose and expression are set per actor.

### Touch another actor

To keep a hand of one actor on another actor (holding hands, a hand on a shoulder):

1. Select the point that should follow, such as the active actor's hand.
2. Under **Contact with another actor**, choose **Other actor** and **Their bone**.
3. Press **Bind Selected Point to This Bone from Here**.

The bind works like any other pin (see [[Hold and bind]]); release it later with **Release from Here**.
It is baked into the exported file. Binds follow one level: an actor bound to a second actor that is
itself bound to a third follows the second actor's own animation only.

### Export

**File → Export SL .anim...** writes one `.anim` per actor, loaded ones included, each baked with that
actor's own bake shape and key reduction. The **Your avatar** bake shape applies to your avatar's actor
only; the others bake on SL Default, unless **Use Your avatar for every actor** is on (viewer only). Naming, the folder and the mirrored copy come from the actor you export from. The
actor's name goes where the pattern has `[ACTOR]`, or at the end of the name when it has none:
`Hug_01_Lead.anim`, `Hug_01_Partner.anim`.

VATs also writes `<name>_placement.txt` beside the files. For each actor it gives the offset, the
rotation and a ready-made line for a sit script:

```
llSitTarget(<0.600, 0.000, 0.000>, llEuler2Rot(<0.0, 0.0, 180.00> * DEG_TO_RAD));
```

## Tips and tricks

- Start all the actors' animations at the same moment in-world; their lengths and loop points already
  match.
- Add the furniture as a [[Props|prop]] to check the placement against it.

## Troubleshooting

### Avatars sit at the wrong height in-world

Second Life does not seat an avatar exactly at the sit target; sit scripts usually correct the height,
often by about 0.4 m. The right value depends on the avatar and the script. Check in-world and adjust Z.

### Only one avatar sits at its offset

`llSitTarget` works in the frame of the prim that holds it. For several avatars, use one sit target per
linked prim, or move each seated avatar to its offset with `llSetLinkPrimitiveParamsFast`
(`PRIM_POS_LOCAL` and `PRIM_ROT_LOCAL` on the avatar's link number).

### The animations drift apart

They were started at different times. Start them together, from one script.

### "Finish the current edit first"

A drag or text edit is still open. Release the mouse or press **Enter**, then try again.

## See also

- [Second Life Wiki: llSitTarget](https://wiki.secondlife.com/wiki/LlSitTarget)

Category: Animating
