# Loop tools

The loop tools fix the usual problems of looping animations: a pop where the loop wraps round, a walk that travels forward when it should walk in place, and a cycle that starts on the wrong pose. They are in **Tools → Loop Tools** and work on the loop range, or on the whole animation when **Loop** is off.

> Related articles: [[Keys and timeline]], [[Graph editor]], [[Retargeting]], [[Motion capture]]

## Usage

The top line of **Tools → Loop Tools** shows the range the tools will work on, for example "Frames 0 to 30 (loop)" or "Frames 0 to 30 (whole clip)". Each command is one undo step.

### Finding a seam

With **Loop** on, a red tick on the timeline at **Loop out** means some channels jump where the loop wraps round (more than 0.5° for a rotation, or 1 mm for a position). Hover the tick for the list: up to 8 channels with the size of the jump, "and N more" past that, and "Tools > Loop Tools > Make Loop Seamless fixes them".

### Making a loop seamless

Choose **Tools → Loop Tools → Make Loop Seamless**. Every channel is made to end exactly where it starts (a rotation may end a whole number of turns away), with the same slope at both ends, so the loop has no pop and no kink. The status bar says "Loop made seamless on N channels", or "The loop was already seamless".

**Blend** (0–15 frames) sets how the correction is made:

- **end key only** (0): only the value at **Loop out** changes.
- 1–15 frames: the last frames ease into the start pose, so the correction is spread out instead of bending the curve sharply at the end.

### Walking in place

**Tools → Loop Tools → Remove Hip Travel (In Place)** takes the forward and sideways travel out of the hips over the loop while keeping their sway and height. The status bar reports what it removed, for example "Removed hip travel: 1.20 m/s forward, 0.02 m/s sideways (1.20 m/s)". That speed is the one an AO's walk should match.

**Add Travel Forward** does the reverse: set the speed in the field beside it (−5 to 5 m/s, 1.00 by default) and press the button to make the hips move forward at that speed.

### Starting the cycle on another pose

Scrub to the frame that should begin the loop, for example a contact pose, then choose **Tools → Loop Tools → Start Cycle at Frame N**. The loop is turned in time so it starts on that pose; the motion itself doesn't change. It is greyed out unless the current frame is inside the loop, not on its first or last frame.

## Tips and tricks

- On walks and runs, run **Remove Hip Travel** first and **Make Loop Seamless** after it.
- Run **Make Loop Seamless** before **Start Cycle at Frame N**: on a loop that isn't seamless, the old seam moves into the middle of the cycle.
- On motion capture and retargeted clips, which have a key on every frame, a **Blend** of several frames spreads the correction instead of bending only the last frame.
- After fixing a seam, check the curves at **Loop in** and **Loop out** in the [[Graph editor]]; the ends should meet with the same slope.
- To mark which part of the clip loops, drag the loop flags on the timeline; see [[Keys and timeline#Looping]].

## Troubleshooting

### The red seam tick stays after Make Loop Seamless

A key at **Loop in** or **Loop out** was changed after the fix, or the loop flags were moved. Hover the tick to see which channels jump, and run **Make Loop Seamless** again.

### Remove Hip Travel reports 0 m/s

The hips have no position keys, or they end where they start. The walk already stays in place.

### Start Cycle at Frame N is greyed out

The current frame is outside the loop, or on its first or last frame. Scrub to a frame inside the loop.

## App and viewer

> **Note:** The loop tools are not yet available in the VATs Animator in the viewer. Loop points and looping playback are.

## See also

- [[Keys and timeline]]
- [[Time editing]]
- [[Onion skin]]

Category: Animating
