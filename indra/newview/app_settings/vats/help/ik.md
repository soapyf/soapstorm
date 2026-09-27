# IK

IK (inverse kinematics) poses a limb by its end: place the hand or foot and the arm or leg bends to reach it. VATs has IK for the arms, legs, fingers, spine, hind legs and wings, and you can switch each limb between IK and FK (bone-by-bone rotation) at any frame without the limb jumping.

> Related articles: [[Posing]], [[Hold and bind]], [[Graph editor]], [[Keys and timeline]]

## Usage

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

**Double-clicking** a bone of a limb also switches it. The status bar confirms, for example "Left Arm is now IK from frame 12".

### Switching back to FK

Select the limb (a bone, the target or the pole) and press **K** again. From that frame on the limb follows its rotation keys again; at the switch it is matched to the IK pose, so there is no jump.

### Keying IK

Moving the target or pole keys it at the current frame, like any bone. **S** (Set Key) with a target or pole selected keys both the target and the pole.

### IK Controls in the Bones tab

The **Bones** tab lists every limb that uses IK under **IK Controls**, as **Left Arm IK** and **Left Arm Pole** and so on. Click one to select it. A limb whose IK is off at the current frame is marked **(FK)** and drawn dimmer.

### IK in the graph

Select a target to see its curves in the [[Graph editor]]: **IK / FK Blend**, which records where the limb switches between IK and FK, and the target's **Translate** and **Rotate** channels. Select a pole to see its **Pole X/Y/Z** channels.

## Tips and tricks

- Use IK for feet on the ground and hands on objects; use FK for swinging arms and free gestures.
- To keep a hand or foot still while the body moves, pin it instead of keying the target on every frame: see [[Hold and bind]]. A pin on the end of a limb drives the limb through IK.
- IK is baked into ordinary rotation keys on export, so the uploaded animation looks exactly as it does in VATs. The `.anim` format has no IK of its own.
- **Select → Select All** also selects the targets and poles of limbs that are in IK.

## Troubleshooting

### "Select a bone of an arm, leg, wing, finger or the spine"

**K** was pressed with no limb bone selected, or with a bone that belongs to no limb (the head, the tail, a face bone). Select a bone of one of the limbs listed above.

### The elbow or knee flips to the other side

The pole decides which way the joint bends. Move the pole out on the side the elbow or knee should point to, and key it where the flip happens.

### The hand stops short of the target

The target is further away than the limb can reach. The limb straightens and points at the target; it does not stretch. Bring the target closer or move the body.

## App and viewer

> **Note:** The viewer has no IK gizmo: pose the limb with the Firestorm Poser, then key the IK target
> and pole from the avatar. See [[VATs Animator (viewer)#Rig tools]].

## See also

- [[Hold and bind]]
- [[Posing]]
- [[VATs Animator (viewer)]]

Category: Animating
