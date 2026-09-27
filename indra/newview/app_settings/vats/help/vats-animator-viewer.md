# VATs Animator (viewer)

The VATs Animator is VATs inside the SoapStorm viewer. It edits and plays an animation on your own
avatar in-world, keys poses made with the Firestorm Poser, and uploads the result without a file round
trip. It shares its core with the standalone app, so both read and write the same `.vat` projects.

> **Note:** The viewer's windows are its own for now. They will be replaced by the app's windows, drawn
> inside the viewer, so the two look and work the same; that is not yet available. This page describes
> the viewer's windows as they are today.

> Related articles: [[VATs]], [[Keys and timeline]], [[Graph editor]], [[Motion capture]], [[Export to Second Life]]

## Usage

The viewer adds three windows to the **Avatar** menu, next to **Poser**: **Avatar → VATs Animator**,
**Avatar → VATs Motion Capture** and **Avatar → VATs Tools**.

### Starting an animation

- **Open...** opens a VATs project (`.vat`) or an SL animation (`.anim`). A Hexton project (`.hxanim`)
  opens too.
- **New from Pose** starts a new animation from the body and hand pose your avatar shows now.
- **New T-Pose** starts a new animation with the body and hands in the T-pose.

### Playing

- **Play** plays the animation on your avatar, and **Stop** stops the preview and gives the avatar back
  to its own animations.
- Click or drag on the timeline to scrub. **Frame** shows the current frame.
- **Loop** makes the animation loop. **Last frame** sets the animation's length in frames (up to 3600).
- **Priority** shows the animation's [[Animation priority|priority]]. The preview plays at that priority,
  so it competes with your AO the way the uploaded animation will.

The preview is local: other residents see nothing until you upload the animation and play it.

**Preview as uploaded** plays the exact `.anim` an upload would send, through the viewer's own animation
decoder. It shows what Second Life does to the keys, but it cannot be scrubbed.

### Keying poses

The VATs Animator has no pose gizmos of its own; you pose with the Firestorm Poser.

1. Scrub to a frame.
2. Open **Avatar → Poser**, start posing yourself and pose the avatar.
3. Press **Key Posed Joints**. Every joint the Poser has changed is keyed at the current frame.

Or select joints in the joint list and press **Set Key** to key them as your avatar shows them now.
**Delete Key** removes the selected joints' keys at the current frame, or every joint's when none is
selected. **Undo** and **Redo** step through your edits.

To key a saved Poser pose, load it in the Poser first, then press **Key Posed Joints**.

### Editing curves

Select joints to see their curves in the graph. Hover over the graph for its controls:

- click a key to select it, **Shift**-click to add to the selection, drag to move (**Shift** locks the
  axis), drag on empty graph to box-select;
- double-click a curve to add a key;
- middle-drag to pan, the wheel to zoom.

**Frame All** fits every shown curve, **Frame Selected** fits the selected keys, and **Delete Keys**
(**Delete**) deletes the selected keys. The tangent buttons **Auto**, **Spline**, **Plateau**,
**Linear**, **Flat**, **Stepped**, **Break** and **Unify** work as in the app (see
[[Graph editor#Shaping curves: tangents]]).

### Saving and exporting

- **Save...** saves a VATs project (`.vat`), which the app opens too.
- **Export .anim...** writes the SL animation file an upload would send.

> **Warning:** **Save...** writes only the animation the Animator shows. Saving over a project made in
> the app drops its other actors (see [[Couples and groups]]) and its project settings, such as the onion
> skin. Save under a new name to keep the original.

### Uploading

1. Type a **Name** and, optionally, a **Description**.
2. Press **Upload (L$N)**, where N is the grid's animation upload cost.
3. Confirm the cost. The animation appears in your inventory when the upload finishes.

The Animator refuses to upload an animation without a name, or one that breaks Second Life's limits (for
example longer than 60 seconds, or 250,000 bytes or more), and says why in its status line.

> **Warning:** Uploading costs L$ and cannot be undone.

### Rig tools

**Avatar → VATs Tools** has the **Rig** tab. It works on the joints selected in the VATs Animator's
joint list, at the Animator's current frame.

- **IK:** pick the limb, then **Switch to IK** or **Switch to FK** (see [[IK]]). The viewer has no IK
  gizmo: pose the hand or foot with the Firestorm Poser, then press **Key IK from Avatar** to key the IK
  target and pole from that pose.
- **Pins:** **Hold in World**, **Bind to** (pick the bone in the list), **Release from Here** and
  **Delete Pin** work as in the app (see [[Hold and bind]]). Pin bands show on the timeline and graph,
  but can't be dragged yet.
- **Starter poses:** the starter hand and body poses from the app's [[Pose library]]. **Apply at Frame**
  keys the pose at the current frame; **Mirrored** applies it to the other side. Your own saved poses are
  not yet available here; load a Firestorm Poser pose instead, then **Key Posed Joints**.

### Motion capture

**Avatar → VATs Motion Capture** receives VMC, Rokoko Studio Live and iFacialMocap like the app (see
[[Motion capture]] and [[Face tracking]]), with the settings on three tabs: **Connect**, **Face** and
**Record**. The live motion drives only your own avatar, at priority 6, and nothing is sent to the
region, so nobody else sees it.

Takes are recorded into the animation open in the VATs Animator; open or create one there first.
**Selected Body Parts Only** uses the joints selected in the VATs Animator's joint list. On the **Face**
tab, **Shape** picks one blendshape and **Strength** sets its strength.

## App and viewer

The two share one core and aim for the same features. The differences today:

| Feature | App | Viewer |
|---|---|---|
| Body | VATs' avatar, or a [[Mesh bodies|devkit mesh body]] | your own avatar and the mesh body you wear |
| Posing | gizmos, [[IK]], [[Hold and bind|pins]], [[Hand poser]] | the Firestorm Poser, then **Key Posed Joints**; IK and pins in **VATs Tools** |
| [[Props]] | imported `.dae` props | none: use in-world objects and worn attachments instead |
| Graph editor | full | no scale box, **Flip Time**/**Flip Values**, **Euler Filter**, **Fit Values**, copy and paste of keys, Frame/Value boxes, all-animated view |
| Open | `.vat`, Hexton `.hxanim`, `.anim`, [[BVH]]; glTF and FBX through retargeting | `.vat`, Hexton `.hxanim`, `.anim` |
| Export | `.anim`, BVH | `.anim`, and direct upload |
| Motion capture | keeps listening when the window closes | closing the window stops listening |
| Export settings | the project's key reduction and export options | defaults; the project's export options are not used yet |

Not yet available in the viewer: the [[Hand poser]], your saved poses, [[Mirror, flip and reverse]],
[[Onion skin]], [[Loop tools]], [[Time editing]], the [[Audio track]], [[Retargeting]], [[Dynamics]], the
[[Ragdoll]], [[Couples and groups]] and BVH export.

## Troubleshooting

### Other people don't see the animation

The preview is local. Upload the animation and play it from your inventory.

### The preview is overridden by my AO

The animation's priority is lower than the AO's animations on those joints. Raise the priority in the
app, or switch the AO off while editing.

### Key Posed Joints says the Poser is not posing you

The status line shows "The Firestorm Poser is not posing you" or "The Poser has not changed any joint
yet". Open **Avatar → Poser**, start posing yourself and move a joint, then press **Key Posed Joints**.

### Motion capture says "Open or create an animation in the VATs Animator first"

Takes are recorded into the Animator's animation. Open one, or press **New T-Pose** in the VATs
Animator.

### The preview says "Not played: your avatar has no joint named ..."

The animation uses a joint your avatar does not have. Check the joint name in the app.

## See also

- [[Motion capture]]
- [[Export to Second Life]]
- [[Project file format]]

Category: Viewer
