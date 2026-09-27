# Hand poser

The hand poser is a small **Hands** window for curling and spreading fingers by dragging one dot per finger, instead of rotating each finger joint. It keys the Bento finger bones at the current frame, for both hands.

> Related articles: [[Posing]], [[Pose library]], [[IK]], [[Skeleton]]

## Usage

### Opening the poser

Press **H** (**Tools → Hand Poser**), or right-click a hand bone and choose **Show Hand Poser**. The **Hands** window has a **Left** half and a **Right** half, each with a dot for **Pinky**, **Ring**, **Middle**, **Index** and **Thumb**, and a palm dot, **All fingers**. Hover a dot to see its name. The bottom line repeats the controls: "Drag down to curl, sideways to spread. Double-click resets."

### Curling and spreading

- **Drag a dot down** to curl that finger.
- **Drag a dot sideways** to spread the finger.
- **Drag the palm dot** to curl and fan the four fingers together (the thumb is left alone).
- **Double-click** a dot to reset that finger, or the palm dot to reset the four fingers.

Each drag is one undo step (**Pose Fingers**; a reset is **Reset Fingers**). The drag keys every joint of the finger at the current frame, so a curl bends all three joints.

### Built-in shapes

For ready-made shapes (fist, point, peace, OK, grips and more), use the starter hand poses in the [[Pose library]]: click one for the left hand, **Shift+click** for the right.

## Tips and tricks

- Show the hand bones (**View → Show Hand Bones**) to see the result on the skeleton while you drag.
- Start from a starter shape, then adjust single fingers with the dots.
- To copy one hand to the other, select the finger bones and press **M** (**Edit → Mirror Bone to Other Side**); see [[Mirror, flip and reverse]].

## Troubleshooting

### Dragging a dot does nothing

The finger is in IK at this frame, and the IK overrides the dots. The status bar says so, for example "Left Index is in IK here, so the IK overrides these dots. Switch it to FK (K) to pose it by hand." Select a bone of that finger and press **K**.

### The Hand pose field did not change the fingers

**Properties → Animation → Hand pose** is different from the hand poser: it picks one of Second Life's built-in hand shapes (Relaxed, Fist, Point and so on), which the viewer applies on top of the animation. It does not move the finger bones in VATs.

## App and viewer

> **Note:** The hand poser is not yet available in the viewer; the starter hand shapes are. See
> [[VATs Animator (viewer)#Rig tools]].

## See also

- [[Pose library]]
- [[Posing]]

Category: Animating
