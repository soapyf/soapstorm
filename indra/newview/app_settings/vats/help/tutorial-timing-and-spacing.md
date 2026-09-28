# Timing and spacing: a head nod

Beginner tutorial 2 of 3. You animate a nod: the head goes down and comes back up in just over half
a second. It is the smallest real animation there is, and it teaches the two ideas every other
animation rests on: **timing** (how many frames a move takes) and **spacing** (how far it goes on each
of those frames). You also meet the [[Graph editor]], where spacing becomes a curve you can see and
change.

> Related articles: [[Your first pose]], [[Keys and timeline]], [[Graph editor]], [[Motion paths]]

## What you will make

![The avatar seen from the side nodding once: the head tips down, pauses, and comes back up, slowing into each end](images/tutorial-timing-and-spacing/nod.gif)
*The finished nod, from the side. The head eases out of rest, eases into the bottom of the nod, and
eases back to rest.*

Three keys on **mHead** over 30 frames (one second at 30 frames per second): straight at frame 0,
down 20° at frame 8, straight again at frame 18. Frames 18 to 30 hold still.

## Steps

### 1. Set up the stand

1. Choose **File → New** (**Ctrl+N**); answer **Don't Save** if it asks.
2. Right-click empty space in the viewport and choose **Poses → Starter poses → Relaxed Stand**.
3. Press **3** (**View → Right**) to see the avatar from the side. A nod reads best in profile.

### 2. Key the starting pose

1. Open the **Picker** tab and click the head: **mHead** is selected.
2. Press **S** (**Edit → Set Key**). The status bar says `Keyed 1 item(s) at frame 0`, and a
   diamond appears at frame 0 on the timeline.

The head has not moved, so why key it? Because a bone's only key sets it on every frame, before and
after. Without a key at frame 0, the nod you key next would already be there at frame 0.

> **Why:** This key is the start of the move. Animators think in **poses at frames**: where the body
> is, and when. Everything between is worked out by the program.

### 3. Key the bottom of the nod

1. Double-click the **Frame** box on the timeline bar (it reads **Frame 0**), type `8` and press
   **Enter**. The playhead moves to 8.
2. In **Properties → Bone**, double-click the second **Rotation** box, type `20` and press **Enter**.

The head tips forward and down, a second diamond appears at 8, and **Properties** says
**Keyed at this frame**. The second box is Rotate Y, which for the head is the nodding axis; the
first box tilts it to the side and the third turns it left and right.

### 4. Key the way back up

1. Go to frame `18` the same way.
2. Set the second **Rotation** box to `0`.

![The Timeline with mHead selected: keys at frames 0, 8 and 18, the playhead at 8, and the ease triangles under the ruler](images/tutorial-timing-and-spacing/timeline-keys.png)
*Three keys. The small triangles under the ruler are **Ease in** and **Ease out**, which are about
how Second Life blends the whole animation in and out; they are not the easing of this lesson.*

### 5. Play it

Press **Space** to play and **Space** again to stop. **Home** jumps to frame 0. The playback repeats
the 30 frames: a nod, a pause while frames 18 to 30 hold, and the next nod.

> **Why (timing):** The head takes 8 frames (0.27 s) to go down and 10 frames (0.33 s) to come back.
> Down is the effort of the gesture, so it is a little quicker; the return is relaxed. Fewer frames
> make a move sharper and more urgent, more frames make it calmer or heavier. Try it: a nod over 4
> and 9 frames reads as an eager "yes!", one over 15 and 35 as a sleepy one.

> **Why (holds):** Frames 18 to 30 are a **hold**: no key changes anything, so the head stays still.
> A hold gives the viewer time to read a gesture before the next one starts. Without it, nod after
> nod runs together into a bobbing head.

### 6. Read the curve

The **Graph** panel below the viewport shows the curves of the selected bone. Click **Rotate Y** in
its list on the left to see only that curve.

![The Graph panel with mHead's Rotate Y curve: flat at frame 0, rising in an S to 20 at frame 8, and falling in an S back to 0 at frame 18](images/tutorial-timing-and-spacing/graph-eased.png)
*Rotate Y: time runs left to right, the angle bottom to top. The curve is flat at every key.*

The curve is flat where it leaves frame 0, flat at the top at frame 8, and flat again as it arrives
at frame 18. A flat curve means the value is hardly changing: the head is moving slowly there. In
the middle of each stretch the curve is steep: the head is moving fast. This slow-fast-slow shape is
called **easing**: the head *eases out* of one pose and *eases in* to the next.

### 7. See the spacing, frame by frame

Go to frame 0 and press **Right** (**Next Frame**) one frame at a time, watching the second
**Rotation** box:

| Frame | 0 | 1 | 2 | 4 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|
| Eased | `0.0°` | `0.9°` | `3.1°` | `10.0°` | `16.9°` | `19.1°` | `20.0°` |
| Linear (from step 8) | `0.0°` | `2.5°` | `5.0°` | `10.0°` | `15.0°` | `17.5°` | `20.0°` |

The eased head moves 0.9° in its first frame and 2.2° in its second, then over 3° a frame in the
middle, then small steps again as it lands. That distribution of the movement over the frames is
**spacing**. Timing says *when* the head arrives; spacing says *how* it gets there.

> **Why (spacing):** Nothing with weight starts or stops instantly. A head, an arm, a car: each speeds
> up out of a still pose and slows down into the next. Small steps near the keys and big steps in the
> middle are what make motion look like it has mass.

> **Why (arcs):** The head turns about the neck joint, so the top of the head travels on a curve, not
> a straight line. Almost every natural motion follows an arc. Because VATs animates rotations, turning
> a joint gives you arcs for free; on bigger moves, such as an arm swing, check them with a
> [[Motion paths|motion path]].

### 8. Try it without easing

1. In the **Graph** panel, drag a box across all three keys of the Rotate Y curve, starting in empty
   space: press left of frame 0 above the curve and release right of frame 18 below it. The keys
   turn yellow.
2. Press **Linear**, the fourth of the curve-shape buttons on the graph's toolbar (two straight lines
   meeting at a key; hover it for its name).

The curve becomes straight lines, and every frame now moves the same 2.5°: the second row of the
table above.

![The same curve made Linear: straight lines from 0 to 20 at frame 8 and back to 0 at frame 18](images/tutorial-timing-and-spacing/graph-linear.png)
*Linear: the same keys, even spacing, sharp corners.*

Play it. The head starts moving at full speed, hits the bottom of the nod and bounces back as if it
had struck something, then stops dead at 18. Compare the two side by side:

![Two nods side by side from the side view, played together: the linear one on the left jerks at each end, the eased one on the right slows smoothly into each pose](images/tutorial-timing-and-spacing/linear-vs-eased.gif)
*The same three keys: Linear on the left, eased on the right.*

### 9. Put the easing back

With the three keys still selected, press **Auto**, the first curve-shape button (a hill, flat on
top). The S-shaped curve returns. (**Ctrl+Z** undoes the Linear change as well.)

**Auto** is the shape VATs gives every new key: smooth, and flat at each peak and valley. Of the
other buttons, **Flat** makes the curve level at a key, for a stronger ease, and **Stepped** holds each
key until the next (useful for planning; see [[Keys and timeline#Blocking and key tags]]). The
**Ease** button applies standard easing shapes to the span between two selected keys; see
[[Graph editor#Easing presets]].

## Check your result

[Open the example](example:tutorial-nod.vat) for the finished nod, and
[Open the example](example:tutorial-nod-linear.vat) for the linear version of step 8. In each, select
**mHead** (**Bones** or **Picker**):

- Press **.** (**Next Key**) from frame 0: the playhead stops at 8, then 18. The second **Rotation**
  box reads `20.0°` at 8 and `0.0°` at 18.
- At frame 2 the second box reads `3.1°` in the eased nod and `5.0°` in the linear one; at frame 12,
  `13.0°` and `12.0°`.
- **Properties → Animation** shows **Frame rate** 30 and **Last frame** 30, with **Loop** off.

Your own nod should read the same.

## Troubleshooting

### The head turns sideways instead of nodding

You typed into the first or the third **Rotation** box. The nod is the second box, Rotate Y. Set the
others back to `0`.

### The nod is already there at frame 0

The key at frame 0 is missing, so the frame 8 key rules the whole clip. Go to frame 0, set the second
box to `0` (this keys it), and play again.

### Linear changed only one key

Only one key was selected. Drag the box again from empty space so that it covers all three keys;
clicking a key selects just that one.

### Check shows 1 on the status bar

The **Check** chip at the bottom reports `Ease in plus ease out (1.60 s) is longer than the animation
(1.00 s)`. That is Second Life's blend-in and blend-out, not the easing of this tutorial; it matters
only when you export, and the next tutorial deals with it.

## Next

[[A breathing idle that loops]]: slow, subtle motion that repeats forever, and your first export.

## See also

- [[Tutorials]]
- [[Keys and timeline]]
- [[Graph editor]]
- [[Motion paths]]: the path a bone travels, with a dot per frame, shows spacing in the viewport

Category: Getting started
