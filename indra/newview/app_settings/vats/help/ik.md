# IK

IK (inverse kinematics) poses a limb by its end: place the hand or foot and the arm or leg bends to reach it. VATs has IK for the arms, legs, fingers, spine, hind legs and wings, and you can switch each limb between IK and FK (bone-by-bone rotation) at any frame without the limb jumping. **Auto IK** does the same for a single drag on any joint, and keys ordinary rotations.

> Related articles: [[Posing]], [[Joint limits]], [[Hold and bind]], [[Balance]], [[Graph editor]], [[Keys and timeline]]

## Usage

### The chain, the target and the pole

![A two-bone chain: root joint, mid joint and end, a wire cube target at the end, a diamond pole under the mid joint, and a dashed arc for the limb's reach](images/ik/two-bone-chain.png)
*Two bones, one target, one pole.*

An IK limb is a chain of two bones between three joints. The **root** (the shoulder or hip) stays where the body puts it. The **end** (the wrist or ankle) goes to the **target**: move the target and the end follows; turn it and the hand or foot turns. The **mid joint** (the elbow or knee) bends about its hinge exactly as far as the target needs, and it points at the **pole**, so the pole decides which way the limb bends. The limb can reach anywhere up to the two bone lengths from the root; a target further away makes it straighten and point at the target, without stretching.

### Limbs with IK

| Limb | Bones | Pole |
|---|---|---|
| Left Arm, Right Arm | shoulder, elbow, wrist | yes |
| Left Leg, Right Leg | hip, knee, ankle | yes |
| Left/Right Thumb, Index, Middle, Ring, Pinky | the three finger joints | yes |
| Spine | `mTorso`, `mChest`, ending at `mNeck` | no |
| Left Hind Leg, Right Hind Leg | `mHindLimb1`–`3` | yes |
| Left Wing, Right Wing | `mWing1`–`3` | yes |

The tail and the face have no IK.

### Switching a limb to IK

1. Select a bone of the limb, for example the wrist.
2. Press **K** (**Tools → Switch IK / FK**, or the **IK / FK** button on the timeline bar). The limb switches to IK from the current frame, matched to its current pose so it does not jump.
3. A **target** box appears at the end of the limb, and for limbs with a pole, a small **pole** marker that shows which way the elbow or knee points. The target is selected.
4. With the **Move** tool, drag the target to place the hand or foot; with **Rotate**, turn it. Move the pole to swing the elbow or knee.

![The right arm in IK: a red wire cube on the wrist and a small red diamond for the pole](images/ik/target-and-pole.png)
*The right arm in IK: the target cube on the wrist, and the pole diamond joined to the elbow by a dashed line.*

**Double-clicking** a bone of a limb also switches it. The status bar confirms, for example "Left Arm is now IK from frame 12".

### Switching back to FK

Select the limb (a bone, the target or the pole) and press **K** again. From that frame on the limb follows its rotation keys again; at the switch it is matched to the IK pose, so there is no jump.

### Keying IK

Moving the target or pole keys it at the current frame, like any bone. **S** (Set Key) with a target or pole selected keys both the target and the pole.

### Auto IK

Auto IK is IK for one drag: drag a joint and the bones above it turn to follow, then it keys them as plain rotations
and leaves no target or pole behind. It is on by default (**Tools → Auto IK**, or the **Auto IK** button on the
timeline bar). Drag a joint by the dot that appears on it when you point at it, with any tool, or with the **Move**
tool's gizmo; see [[Posing#Dragging a joint in the air (Auto IK)]].

![The mech's hind foot dragged through the air by its dot: the hind leg lifts, swings forward and bends at its own hinges](images/ik/mech-leg-drag.gif)
*A hind foot dragged through the air on the test mech: the whole leg follows.*

| Dragged joint | Bones that follow by default |
|---|---|
| A wrist | elbow, shoulder and collar |
| An ankle (or a foot, a toe) | knee and hip (and the ankle, the foot) |
| A hind leg's foot | the hind leg to `mHindLimb1` |
| A wing tip, a tail tip, a fingertip | its wing, tail or finger to the first bone |
| Any other joint | the two bones above it |

- **Longer or shorter.** While you drag, the mouse wheel or **]** takes one more bone, **[** one fewer; the status
  bar shows the count. VATs remembers the length for that joint until it closes. Bones taken beyond the default only
  move when the limb cannot reach on its own, as with a target's **Pull**: an arm lengthened to the chest bends at
  the elbow first and leans the chest only for the rest.
- **What it never takes.** The pelvis; a bone a pin holds (the chain stops below it, so the pinned joint stays
  exactly where it is); a limb in IK at this frame, whose bones its target drives. Bento's `mSpine1`–`4` are passed
  through but never turned or keyed.
- **Hinges and limits.** Elbows, knees, the hind legs' second and third joints, the wings' second joint and the finger joints
  bend only about their hinge, and never through straight to the other side. On a [[Mesh bodies|mesh body]] with rig
  axes the hinge is the body's own axis; on a body rigged with a limb already bent 20° or more, the hinge is the
  axis of that bend; otherwise it is Second Life's. When **Tools → Respect Joint Limits** is on, Auto IK and
  two-bone IK clamp joint motion to configured limits. See [[Joint limits]].
- **Pins and IK.** A pinned joint moves its pin offset instead, as with any Move drag ([[Hold and bind]]). A limb in
  IK is posed by dragging its target. Mirror, when on, keys the other side too.

Auto IK or IK / FK? Auto IK is for posing: fast, any chain, and the result is ordinary keys you can tweak bone by
bone. Switch a limb to IK (**K**) when its hand or foot must stay on a spot over many frames, or when you want to key
the end's path and let the elbow or knee work itself out.

### Pose by dragging the body

Click and drag directly on the avatar's body in the view to pose it without gizmos (the **Select** and **Move**
tools; with **Rotate** a pressed bone turns instead):

- **Mesh dragging.** Click and drag any limb or body part: the point you pressed follows the pointer, and Auto IK turns the bones above it. The hovered bone is picked from skin weights. The wheel and **[ ]** change how many bones above the pressed one turn, as in any Auto IK drag.
- **Whole-body drag.** Dragging the pelvis or torso moves the hips; dragging the chest leans `mTorso` halfway and the hips take the rest. Feet the shown body stands on (within 5 cm of its floor, measured by the mesh's skin on a mesh body) stay where they are: each leg is solved by Auto IK and keyed as plain rotations, with no IK controller. Pins and limbs in IK hold their feet as always. Small markers appear under planted feet during the drag. Hold **Alt** during the drag to let the feet move with the body, the legs as they were at the press; in the Second Life and Industry presets **Alt** with the click is the camera, so press first.
- **Follow-through.** Tail, wing and ear chains lag behind and settle, using the Dynamics solver (**Tools → Follow-Through While Posing**, default on). Only the dragged bones are keyed when you release the mouse.
- **Switching IK / FK keeps the pose.** On a limb the shown body bends off SL's hinge (hind legs, knees rigged bent, bones with their own axes), **K** solves with Auto IK's solver in the plane the limb bends in, so switching doesn't move it. Those limbs bake against the bend planes of the bake shape's body, so export with the body you posed on.

### Full-body reach

A hand target dragged further than the arm reaches normally leaves the hand short. Give the target a **Pull** and the body goes after it instead.

1. Select the target (**Left Arm IK**, **Right Leg IK** and so on). **Properties → Bone** shows the target's name and a **Pull** slider, 0 to 1, 0 by default. Arms and legs have it; fingers and the spine do not.
2. Set **Pull**, for example `1`. It is saved with the project, per target.
3. Drag the target out of reach with the **Move** tool and let go.

When you let go of an arm's target past the arm's full length, the spine leans towards it first, up to 30°, through the spine's own IK solve, as far as it takes to bring the shoulder within reach. What is still missing after the lean, times **Pull**, moves the hips straight towards the target. With **Pull** `1` the hand reaches the target; with `0.5` the hips go half the remaining way and the hand stops short by the rest. A leg's target skips the lean and moves only the hips. The status bar says, for example, "Hips moved 12.4 cm to reach".

The result is ordinary keys at the current frame: rotation keys on `mTorso` and `mChest` (or the **Spine** target when the spine is in IK) and a position key on `mPelvis`, in the same undo step as the drag. Feet held by [[Hold and bind|pins]] stay where they are while the hips move; feet that are not pinned travel with the hips. A target within reach, a rotate drag and **Pull** `0` change nothing but the target.

### IK Controls in the Bones tab

The **Bones** tab lists every limb that uses IK under **IK Controls**, as **Left Arm IK** and **Left Arm Pole** and so on. Click one to select it. A limb whose IK is off at the current frame is marked **(FK)** and drawn dimmer.

![The IK Controls group at the top of the Bones tab, listing Right Arm IK and Right Arm Pole](images/ik/ik-controls.png)
*The **IK Controls** group of the **Bones** tab.*

### IK in the graph

Select a target to see its curves in the [[Graph editor]]: **IK / FK Blend**, which records where the limb switches between IK and FK, and the target's **Translate** and **Rotate** channels. Select a pole to see its **Pole X/Y/Z** channels.

### Worked example: reaching with the right arm

[Open the example](example:ik-reach.vat): the right arm is in IK from frame 0, with the upper arm out to the side and the forearm up. The target is keyed at frame 0 and again at frame 24, 12 cm further in towards the head and 8 cm higher. The **Bones** tab lists **Right Arm IK** and **Right Arm Pole** under **IK Controls**.

1. Scrub from 0 to 24: the hand follows the target and the elbow bends further to let it. The shoulder and elbow have no keys after frame 0; the IK poses them.
2. Go to frame 12, click **mElbowRight**, and press **K**. The status bar says "Right Arm is now FK from frame 12" and the target cube disappears.
3. Scrub to 24: the hand no longer moves. From frame 12 on the arm follows its rotation keys, and the switch keyed the pose it had at that moment, so there was no jump.
4. **Edit → Undo** puts the arm back in IK.

## Tips and tricks

- Use IK for feet on the ground and hands on objects; use FK for swinging arms and free gestures, posed quickly by
  dragging the hand with Auto IK.
- An Auto IK drag solves each step from the last, so the chain follows the path you drag along. To get a different
  bend, drag the joint round the other way, or turn the elbow or knee afterwards with the Rotate tool.
- To keep a hand or foot still while the body moves, pin it instead of keying the target on every frame: see [[Hold and bind]]. A pin on the end of a limb drives the limb through IK.
- IK is baked into ordinary rotation keys on export, so the uploaded animation looks exactly as it does in VATs. The `.anim` format has no IK of its own.
- **Select → Select All** also selects the targets and poles of limbs that are in IK.

## Troubleshooting

### "Select a bone of an arm, leg, wing, finger or the spine"

**K** was pressed with no limb bone selected, or with a bone that belongs to no limb (the head, the tail, a face bone). Select a bone of one of the limbs listed above.

### The elbow or knee flips to the other side

The pole decides which way the joint bends. Move the pole out on the side the elbow or knee should point to, and key it where the flip happens.

### The hand stops short of the target

The target is further away than the limb can reach. The limb straightens and points at the target; it does not stretch. Bring the target closer, move the body, or give the target a **Pull** so the body follows it (see [[IK#Full-body reach]]).

## App and viewer

> **Note:** In the viewer the IK handles and poles are drawn on your avatar in the world and drag the same
> way. See [[VATs Editor (viewer)#The world as the view]].

## See also

- [[Hold and bind]]
- [[Balance]]
- [[Posing]]
- [[Joint limits]]
- [[VATs Editor (viewer)]]

Category: Animating
