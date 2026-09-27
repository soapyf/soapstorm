# Couples and groups

A couples or group animation is a set of animations, one per avatar, made to play together on one
piece of furniture or pose ball: a dance for two, a hug, a group photo. VATs keeps all the avatars
("actors") in one project on one timeline, places each one relative to the shared sit target, and
exports a file per actor plus a note with the sit-target values.

> Related articles: [[Hold and bind]], [[Export to Second Life]], [[Props]], [[Mesh bodies]], [[Keys and timeline]]

> **Note:** In the viewer your own avatar shows the actor you edit, and the other actors are skeletons
> around it, seen only by you. See [[VATs Editor (viewer)#The world as the view]].

## Usage

### Add actors

Open **Tools → Actors (Couples and Groups)...**. A project starts with one actor. Add another with:

- **Add Partner**: a mirrored copy of the current actor's animation, 0.6 m in front of it and facing
  it;
- **Duplicate**: a copy of the current actor's animation, 0.8 m to its side;
- **Blank**: an actor with no keys, 0.8 m to the side.

Each new actor gets its own colour. Cross-actor binds are not copied.

### Choose the actor to edit

Click an actor's body in the view, or its name in the **Actors** window. The Bones list, the graph and
keying all work on that actor; the others are shown dimmed in their colour. A locked actor cannot be
chosen.

The buttons on each row act on the other actors:

- **place** shows a gizmo on that actor in the view. The Move tool moves it; the Rotate tool turns it
  about the vertical axis.
- **hide**/**show** hides the actor in the view.
- **lock**/**unlock** stops the actor from being chosen or placed by accident.

Right-click an actor's name for **Delete Actor**. Binds from other actors to it are removed as well.
With one actor left, the project is an ordinary single-avatar project.

### Set up the active actor

Below the list, under the actor's name:

| Field | Meaning |
|---|---|
| **Name** | Used in the exported file names. Press **Enter** to apply. |
| **Colour** | The tint when the actor is not being edited. |
| **Body** | **Same as the view** (follows **View → Body**), a Linden body, or one of your [[Mesh bodies]]. |

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

**File → Export SL .anim...** writes one `.anim` per actor, each baked with that actor's own bake shape and
key reduction. Naming, the folder and the mirrored copy come from the actor you export from. The
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
