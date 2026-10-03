# Joint limits

Joint limits stop a bone from turning past where a body can anatomically or mechanically go. They constrain posing across the Rotate gizmo, Auto IK, two-bone IK and whole-body drags, preventing hyperextension (such as bending knees backwards) and self-penetration (pushing thighs through the belly). Limits are saved in the project (`.vat`) per body, and never change the exported Second Life animation file format.

> Related articles: [[Posing]], [[IK]], [[Mesh bodies]], [[Interface]], [[Balance]]

## Usage

### Respecting joint limits

When **Tools → Respect Joint Limits** is checked (the default; the **Limits** button on the toolbar toggles it too), interactive posing stops bones at their boundaries:

- **Rotate gizmo:** dragging the rotation rings stops turning when the bone reaches its hinge or cone limit.
- **Auto IK:** dragging a joint dot works around constrained bones. If an intermediate joint hits its limit, the remaining bones in the chain continue reaching for the target. Drag past where the limits let the limb reach and it stops at the nearest point it can, and follows smoothly when you come back.
- **Two-bone IK:** the knee or elbow turns toward its pole only as far as the hip or shoulder cone allows, so the foot or hand still lands on the target.
- **Body drag:** whole-body posture adjustments respect planted feet and limb limits simultaneously.

Typed rotation values in the **Properties** panel are never clamped, leaving deliberate overrides available. When a bone is posed past its limits, its **Rotation** numbers highlight in orange to warn of the violation.

### Visual badges and clamp feedback

Joint limits provide clear visual feedback across the interface:

- **Everywhere a bone is shown:** joints with applied limits show a small solid ring or badge on their joint dots in the 3D Viewport, the **Bones** list, and the [[Picker]]. While **Suggest Joint Limits** is open, joints with suggestions not yet applied show a hollow mark.
- **Dimmed when inactive:** when **Tools → Respect Joint Limits** is toggled off, all limit rings and badges appear dimmed, indicating limits exist but are not currently restricting poses.
- **Live clamp feedback:** when a limit stops a joint during posing (Auto IK, gizmo rotation, body drag, or K-IK), the joint dot and its limit widget flash in the warning accent color, and the status bar reports which stop was reached:
  `mKneeLeft stopped at 140 degrees (hinge max)`

### Setting limits by hand in the viewport

Adjust any joint's limits directly in the 3D view using interactive handles:

1. Select the bone in the viewport and press **L** (or click **Edit Limits (L)** in the **Properties** panel). A joint without a limit gets one from its suggestion, widened to cover its pose.
2. The joint's limit widget appears with interactive drag handles:
   - **Hinge:** drag either end of the colored arc to change **Min angle** and **Max angle**. Drag the dot on the short axis line to re-aim the hinge bending axis. A needle indicates the bone's current pose angle.
   - **Cone:** drag the dot on the base rim to widen or narrow the swing cone angle. Drag the second rim dot to tilt the cone's direction. A suggested cone can stop sooner in some directions than others (a thigh stops where it meets the belly but swings freely out to the side), and its rim dips there; dragging the rim widens or narrows every direction together.
   - **Twist:** drag either end of the smaller twist arc to adjust allowed axial roll.
3. A drag snaps to the angle snap step while snapping is on (hold **Ctrl** in presets other than Second Life). Each drag is one undo step (**Ctrl+Z**), and **Esc** or a right-click during a drag puts the limit back as it was.
4. **Mirror:** with **Mirror** checked in **Properties** (on by default), every edit to a left or right joint gives the other side the mirror image (editing `mKneeLeft` updates `mKneeRight`). This covers the handles, the number fields, **Kind**, **Hinge axis**, **Set From Pose**, **Clear** and the quick fixes below.

### Editing limits in Properties

The **Joint Limits** section of the **Properties** panel shows the selected bone's applied limit:

- **Kind:** **None (Unlimited)**, **Hinge** or **Cone (+ Twist)**. A new hinge or cone starts from the joint's own suggestion.
- **Aim at Pose:** bend the joint the way it should move (turn **Limits** off on the toolbar first), then click: a hinge turns to bend that way, a cone centres on the pose.
- **Flip** (hinges): the same range, bending the other way. Use it on a knee or elbow that bends backwards.
- **Turn 90°** (hinges): turns the hinge a quarter turn around the bone, so bending forward becomes bending sideways.
- **Set From Pose:** widens the limit just enough to cover the bone's current pose.
- **Clear:** removes the limit.
- **Copy to Other Side:** gives the matching joint on the other side the mirror image of this limit.
- **Hinge axis**, **Min angle**, **Max angle**, **Cone angle**, **Twist min**, **Twist max:** exact values, each change one undo step.

### Reviewing with Suggest Limits

To configure limits across an entire body at once, choose **Rig → Suggest Joint Limits...**. The **Suggest Joint Limits** window opens against the right of the view, beside **Properties**, and can be docked anywhere. The suggestions are for the body shown: switch to another actor or mesh body, or open another project, and the window suggests again for that body.

Suggestions start from ordinary human ranges, then fit the body: knees and elbows rigged bent (a creature's hind legs) hinge in the plane they bend in, and each bone is swung until it meets the rest of the body, so a thick torso stops the thigh sooner. Left and right always come out the same.

#### The limb tree and selection

Joints are grouped by limb:
- **Spine and Neck, Head**
- **Left Arm, Right Arm, Left Hand, Right Hand**
- **Left Leg, Right Leg, Hind Legs** (for quadruped and creature bodies)
- **Tail, Wings**

Face bones are left free, so they never appear. A group shows how many of its joints differ from the applied limits, for example **Left Leg (5) [5 chg]**.

Work with the tree:
- **Checkboxes:** tick or untick limb groups or individual bones to choose which suggestions the **Apply** buttons write.
- **Find in view:** clicking any bone in the 3D viewport selects and reveals it in the list.
- **Multi-selection:** **Shift**-click selects every row between the last clicked row and this one; **Ctrl**-click adds or removes one joint.
- **Other Side:** swaps the selection to the matching joints on the other side.
- **Filter chips:** click **Low Conf**, **Changed**, or **Not Applied** to show only joints that need review.

#### Live posing preview

Suggestions sit in a separate pending set beside your applied limits:
- **Preview** switches what posing uses while the window is open: **Off** (no limits), **Suggested**, or **Applied**. Nothing is committed. With **Respect Joint Limits** off, posing ignores every preview, and the window says so.
- Choose **Suggested** and drag limbs with Auto IK or the Rotate gizmo to test whether the ranges feel natural before applying them.
- With **Suggested**, the viewport's handles and **Set From Pose** edit the suggestions, not the applied limits. **Ctrl+Z** and **Ctrl+Y** undo and redo these edits too, in turn with your other edits.

#### Per-joint testing

Each joint row has three buttons:
- **Focus** (a dot in a frame): eases the view onto the joint and selects it. A hinge is seen along its axis from the outside of the body, so the arc faces you (a right knee is seen from the right, not through the other leg).
- **Test** (play): sweeps the joint through its range and back, exposing a wrong bending axis or inverted stops.
- **Reset** (circular arrow): puts the joint back to its suggestion (one undo step).

#### Committing changes

Apply the suggestions you want:
- **Apply Selected (n):** writes the selected, checked joints into the body's limits.
- **Apply \<group\> (n):** writes the checked joints of the selected joint's group, for example **Apply Left Leg (5)**.
- **Apply All (n):** writes every checked joint.
- **Discard:** drops the suggestions and closes the window; posing returns to the applied limits.

Each count, and the status message after applying, counts only joints whose limit actually changes. Each **Apply** is one undo step.

## Configuration

Joint limits are enabled or disabled globally via **Tools → Respect Joint Limits** (on by default). The setting is remembered in user preferences. Limits are stored per body in the project file: the Second Life avatar's limits (every shape, under `sl-default`) and each mesh body's own.

## Tips and tricks

- Open **Rig → Suggest Joint Limits...** after importing any new mesh body to inspect its automatically calculated limits.
- Use **Test** on knee and elbow hinges in the review window: if the sweep bends the limb backward, select the joint and click **Flip** in **Properties** (or drag the hinge's limit handles in the view).
- Choose **Preview: Suggested** and grab a foot or hand with Auto IK to verify range of motion before applying.
- Keep **Mirror** checked when adjusting arm or leg handles so you only have to tune one side of the body.

## Troubleshooting

### Rotation field turns orange

The bone's current rotation exceeds its configured joint limit.

- Click **Set From Pose** in **Properties** to widen the limit to include this rotation.
- Or rotate the bone back within its allowed arc in the viewport.
- Or click **Clear** in the **Joint Limits** section to remove the constraint.

### Auto IK stops before reaching the pointer

A joint in the chain has reached its limit and stopped bending. Auto IK redistributes rotation to the other joints in the chain, but once the entire chain reach is constrained, it will not push bones past their limits.

- Look for the warning accent flash on the stopped joint or check the status bar for `mKneeLeft stopped at 140 degrees (hinge max)`.
- Drag the viewport limit handle on the stopped joint to widen its range.
- Or temporarily toggle off **Tools → Respect Joint Limits** to pose freely.

### A knee or elbow bends the wrong way

The hinge's axis points the other way. Select the joint and click **Flip** in **Properties**; or bend it the right way with **Limits** off and click **Aim at Pose**.

## See also

- [[Posing]]
- [[IK]]
- [[Mesh bodies]]
- [[Interface]]
- [[Balance]]

Category: Animating
