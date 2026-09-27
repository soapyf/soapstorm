# Time editing

Time editing changes when things happen rather than what the pose is: open empty frames, cut a stretch out, make a section faster or slower, or copy a section of keys to another point in time. The commands are in **Edit → Time** and in the timeline's right-click menu.

> Related articles: [[Keys and timeline]], [[Graph editor]], [[Loop tools]], [[Couples and groups]]

## Usage

### Picking a range

Most commands work on a frame range. **Shift+drag** along the timeline to mark one; it shows as a yellow band, and a plain click on the timeline clears it. Without a timeline range, the span of the keys selected in the [[Graph editor]] is used instead.

With no range, the range commands are greyed out with "Shift-drag a frame range on the timeline first".

### Which keys move

- **With bones selected**, only their keys move, together with their pins and IK controls.
- **With nothing selected**, the whole animation moves: every bone, and the animation's length, loop points and pins follow.

The status bar ends with **(all bones)** or **(selected bones)** so you can tell which happened. Keys that end up past the last frame extend the animation. Each command is one undo step.

### Inserting frames

**Edit → Time → Insert Frames...** opens empty frames at the playhead. The dialog reads "Insert empty frames at frame N on every bone" (or "on the selected bones"); set **Frames** (1–3600, 10 by default) and press **OK**. Keys at or after the playhead move later by that many frames.

### Removing a range

**Edit → Time → Remove Range** deletes the frames of the range and closes the gap: keys after the range move earlier. The range is cleared afterwards.

### Stretching a range

**Edit → Time → Stretch Range...** makes the range longer or shorter. The dialog reads "Frames a to b (n frames) become:"; set the new length in **Frames** and press **OK**. Keys inside spread out or bunch up, and later keys move by the difference.

### Copying and pasting a range

1. Mark a range and choose **Edit → Time → Copy Range**. The status bar says "Copied frames a to b".
2. Move the playhead to where the copy should go.
3. Choose one of:
   - **Paste Range**: pastes at the playhead, replacing the keys already there over the length of the copy.
   - **Paste Range, Inserting**: opens room at the playhead first, so nothing is overwritten and later keys move later.
   - **Paste Range Mirrored**: pastes with left and right swapped, as [[Mirror, flip and reverse|Mirror]] does.

The paste commands are greyed out with "Copy a range first" until something is copied. A paste goes onto the same bones it was copied from, whatever is selected when you paste.

## Tips and tricks

- To slow down one move, mark its frames and **Stretch Range...** to a larger number; to speed it up, a smaller one.
- Repeat a gesture by copying its range and pasting it later with **Paste Range, Inserting**.
- A walk's second step is often the first step mirrored: copy the first step's range and use **Paste Range Mirrored** half a cycle later.
- Select only the arms before stretching to change their timing while the legs keep theirs.
- In a scene with several avatars, time edits keep every actor the same length and loop; see [[Couples and groups]].

## Troubleshooting

### A time edit moved every bone, not just mine

Nothing was selected when you ran it, so the whole animation was edited. Undo (**Ctrl+Z**), select the bones, and run it again. The status bar's **(all bones)** or **(selected bones)** shows which happened.

### Insert Frames made the animation longer than 60 seconds

Inserting on every bone extends the animation. Check the seconds under **Last frame** in **Properties → Animation**; Second Life refuses animations over 60 seconds.

## See also

- [[Keys and timeline]]
- [[Loop tools]]
- [[Audio track]]

Category: Animating
