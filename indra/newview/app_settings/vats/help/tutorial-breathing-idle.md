# A breathing idle that loops

Beginner tutorial 3 of 3. You make a standing idle: the avatar breathes in and out, once every four
seconds, forever. It is the kind of animation an AO plays when you are doing nothing, so it has to be
calm enough to watch for minutes and to repeat without anyone seeing where it starts again. At the
end you export it as an `.anim` file ready to upload to Second Life, and read what the file costs.

> Related articles: [[Timing and spacing: a head nod]], [[Keys and timeline#Looping]], [[Animation priority]], [[Export to Second Life]], [[Idle layer]]

## What you will make

![The avatar from the side, close on the chest and head, breathing once: the chest lifts and the shoulders rise slightly, then settle](images/tutorial-breathing-idle/breathing.gif)
*One breath, four seconds, looping. The chest lifts 4°, the collarbones rise 3°, and the head
dips 2° to keep looking straight ahead.*

A 120-frame loop at 30 frames per second on the Relaxed Stand: four bones keyed at frames 0, 48 and
120, with frame 120 a copy of frame 0. Priority 2, exported as a 663-byte `.anim`.

## Steps

### 1. Set the length and the loop

1. Choose **File → New** (**Ctrl+N**); answer **Don't Save** if it asks.
2. Right-click empty space in the viewport and choose **Poses → Starter poses → Relaxed Stand**.
3. In **Properties → Animation**, double-click **Last frame**, type `120` and press **Enter**. The
   line under it reads **4.00 seconds**.
4. Tick **Loop**. **Loop in** and **Loop out** appear, and the loop band is tinted on the timeline.
5. Set **Loop out** to `120`. It shows `30` at first: a new document's loop ends at frame 30, and
   lengthening the animation does not move it.

> **Why:** People at rest breathe 12 to 20 times a minute, so one breath takes 3 to 5 seconds. Four
> seconds is calm without looking sleepy. The whole loop is one breath, so it repeats with no seam
> in the middle.

### 2. Set the priority

Set **Priority** to `2`.

![Properties, Animation section: Frame rate 30, Last frame 120 (4.00 seconds), Loop ticked, Loop in 0, Loop out 120, Priority 2](images/tutorial-breathing-idle/animation-settings.png)
*The settings after steps 1 and 2.*

Second Life plays several animations on an avatar at once. When two of them move the same bone, the
one with the higher **priority** (0 to 6) wins that bone. An idle is the bottom layer: anything else
you play, a gesture, a dance, a sit, should win over it. 2 is a usual priority for stands; see
[[Animation priority]] for the whole scale.

### 3. Key the out-breath at frame 0

1. Open the **Picker** tab. Click the **Chest** (the third dot up the spine), then **Shift+click**
   both collars (the dots either side of the spine at shoulder height, where the arm lines start) and
   the **Head** (the top dot). Four bones are drawn in the accent colour.
2. Press **S** (**Edit → Set Key**). The status bar says `Keyed 4 item(s) at frame 0`.

This is the resting pose: breath out, everything as Relaxed Stand left it.

> **Why:** Key only the bones you mean to move. An `.anim` claims every bone it has keys for, at its
> priority, for as long as it plays. This idle leaves the legs and hips alone, so a walk or a sit can
> take them without a fight.

### 4. Key the in-breath at frame 48

1. Go to frame `48` (double-click the **Frame** box, type `48`, **Enter**).
2. Click the **Chest** in the picker (a plain click selects just it) and set the second **Rotation**
   box to `-4`. The chest tips back a little: the rib cage lifts.
3. Click the left collar (on your right on the **Front** view) and set the first **Rotation** box
   to `-2`. It read `-5`, so the shoulder rises 3°.
4. Press **M** (**Edit → Mirror Bone to Other Side**): the right collar gets `2`, the mirror image.
5. Click the **Head** and set the second **Rotation** box to `2`.

> **Why:** Breathing in takes less time than breathing out, so the in-breath is 48 frames (1.6 s) and
> the out-breath 72 (2.4 s). Uneven timing is one of the things that keep a loop from feeling like a
> machine.

> **Why (the head):** When the chest tips back it carries the neck and head with it, and the avatar
> would stare at the ceiling on every breath. Tipping the head 2° forward cancels that, so the eyes
> stay level. Correcting one part for the motion of another is called *counter-animation*.

### 5. Close the loop at frame 120

The loop has to end exactly where it starts, or the avatar jumps once every four seconds.

1. Select the four bones again: click the **Chest**, **Shift+click** the collars and the **Head**.
2. Press **Home** to go to frame 0, and with the pointer over the viewport press **Ctrl+C**
   (**Edit → Copy Pose**). The status bar says `Copied 4 item(s)`.
3. Press **End** to go to frame 120, and press **Ctrl+V** (**Edit → Paste Pose**). The status bar says
   `Pasted the pose at frame 120`.

The timeline shows keys at 0, 48 and 120.

> **Tip:** With the pointer over the **Graph** panel, **Ctrl+C** and **Ctrl+V** copy and paste the
> selected *keys* instead of the pose. Keep the pointer over the viewport for this step.

### 6. Play and watch the seam

Press **Space**. The avatar breathes in, breathes out, and starts again with no jump. Let it run for a
few cycles and watch only the moment it passes frame 120: nothing should happen there.

Select only the **Chest** and look at its curve in the **Graph** panel:

![The Graph panel with mChest's Rotate Y: a smooth dip from 0 at frame 0 down to -4 at 48 and back to 0 at 120, drawn faintly again before 0 and after 120](images/tutorial-breathing-idle/graph-chest.png)
*One breath as a curve. The faint parts before frame 0 and after 120 are the loop going round again.*

The curve leaves frame 0 flat and arrives at 120 flat, and because both ends are the same value the
loop joins without a kink. The faint copies either side show how the next cycle continues (VATs draws
them because **Tools → Loop Tools → Loop-Aware Tangents** is on in new projects; see
[[Loop tools#Loop-aware tangents]]).

> **Why:** A good idle is almost invisible. The changes here are a few degrees; if the viewer notices
> the breathing, it is too big. Subtle motion is still motion, though: a stand with no breath at all
> looks frozen next to one that has it.

### 7. Read the upload size

1. Press **Ctrl+E** (**File → Export SL .anim...**). The top line reads
   `Length 4.00 s, priority 2, looping, ease 0.30 / 0.30 s`.
2. Type `Breathing` into **Name**. **Saves as** changes to `Breathing_01.anim`.

![The Export SL .anim window: Length 4.00 s, priority 2, looping; Reduce keys Per bone; Upload size 663 / 250,000 bytes, 4.00 / 60 s, and the table of parts](images/tutorial-breathing-idle/upload-size.png)
*The upload size of the breathing idle.*

**Upload size**, in the middle of the window, measures the file before you write it:

- `663 / 250,000 bytes`: the file is 663 bytes. Second Life refuses a file of 250,000 bytes or more,
  so this one uses well under 1% of what it may.
- `4.00 / 60 s`: the length against Second Life's 60-second limit. Both bars turn amber from 90% of a
  limit and red over it.
- The table lists the body parts that cost the most. **Right Arm (3)** and **Left Arm (3)** are the
  collar, shoulder and elbow of each arm, keyed by Relaxed Stand; **Torso (1)** is the chest and
  **Head (1)** the head. Eight bones in all.

> **Why:** Size grows with the number of bones keyed and the number of keys each needs. A few keys
> on a few bones, like this idle, cost almost nothing; motion capture keyed on every frame of every
> bone can pass the limit. See [[Export to Second Life#Check the upload size]].

### 8. Export

Press **Export SL .anim**. The first time, a dialog asks where to save; the folder you pick becomes
the export **Folder**, and the file takes the name **Saves as** shows unless you type another there. The status bar says
`Exported Breathing_01.anim to <folder>: 8 bones, 4.00 s, priority 2, 663 bytes`.

**Ease in** and **Ease out** (0.30 s each) are how long Second Life takes to blend this animation in
when it starts and out when it stops, so the avatar never snaps into or out of the breath. For a
looping idle the defaults are fine.

Upload the file as in [[First steps#10. Upload]]. The loop, the priority and the ease times travel
inside the `.anim`.

## Check your result

[Open the example](example:tutorial-breathing-idle.vat) and compare:

- **Properties → Animation**: **Last frame** 120, **Loop** ticked, **Loop in** 0, **Loop out** 120,
  **Priority** 2.
- Select **mChest** and press **.** (**Next Key**) from frame 0: keys at 48 and 120. At 48 its
  **Rotation** reads `0.0°`, `-4.0°`, `0.0°`; at 120 all zero again.
- At frame 48, **mCollarLeft** reads `-2.0°` on X, **mCollarRight** `2.0°`, and **mHead** `2.0°` on Y.
  At 0 and 120 the collars read `-5.0°` and `5.0°`.
- **Ctrl+E** shows `663 / 250,000 bytes` and `4.00 / 60 s`.

## Troubleshooting

### Playback repeats only the first second

**Loop out** is still 30. Set it to `120` in **Properties → Animation**. While **Loop** is ticked,
playing in VATs repeats only the loop band, as Second Life will.

### The avatar jumps at the end of each breath

Frame 120 does not match frame 0: a key is missing at 120, or one bone was not selected when you
pasted. Select the four bones at frame 0, **Ctrl+C**, go to 120, **Ctrl+V**. A red tick on the timeline
at **Loop out** marks a seam; hover it for the channels that jump, and see [[Loop tools]].

### Ctrl+V pasted keys at the wrong frame

The pointer was over the **Graph** panel, which pastes keys at the current frame. **Ctrl+Z**, move the
pointer over the viewport and paste again.

### Breathing is too strong or too weak

Change the values at frame 48 only; frames 0 and 120 are the rest pose. Keep the chest within a few
degrees for a calm idle; much more reads as heavy breathing, which suits a different animation.

### The arms or legs move in Second Life when they should not

Another animation is playing on those bones with a higher priority, which is expected: the idle is
meant to lose. If this idle wins bones it should not, key fewer bones rather than raising priority.

## Next

The beginner tutorials end here. Continue with the [[Tutorials#Routine]] tutorials, or try the
[[Idle layer]], which adds breathing and sway to any stand without keys.

## See also

- [[Tutorials]]
- [[Keys and timeline]]
- [[Loop tools]]
- [[Animation priority]]
- [[Export to Second Life]]

Category: Getting started
Order: 12
