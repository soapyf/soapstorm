# Pose library

The pose library holds poses and clips you can reuse in any project: your own saved poses and clips, and a set of starter hand and body poses that come with VATs. It lives in the **Poses** and **Starter poses** sections of the **Inventory** tab.

> Related articles: [[Posing]], [[Hand poser]], [[Mirror, flip and reverse]], [[Keys and timeline]]

## Usage

### Saving a pose

1. Go to the frame with the pose.
2. Select the bones to save, or select nothing to save the whole body.
3. Press **Save Pose...** in the **Poses** section of the **Inventory**, or right-click in the view and choose **Save Pose...**.
4. Type a name and confirm.

A body part's right-click menu also has **Save** *part* **Pose...**, which saves just that part.

### Saving a clip

A clip is the keys of some bones over a frame range, for example a head nod or a hand gesture.

1. Select the bones.
2. **Shift+drag** a frame range on the timeline, or select keys in the [[Graph editor]].
3. Press **Save Clip...** in the **Inventory**, or choose **Edit → Save Clip of Selected Bones...**.

**Save Clip...** is greyed out until both are done; its tooltip reads "Select bones, then Shift-drag a frame range on the timeline or select keys in the graph". A body part's right-click menu offers **Save** *part* **Clip** for the picked range.

### Using saved poses and clips

Your items are listed under **Poses** and **Clips**, with a thumbnail where one exists or an icon for the kind; clips carry a small play mark.

- **Double-click** an item to apply a pose, or paste a clip, at the current frame.
- **Drag** an item onto the view to do the same.
- Tick **Apply mirrored** to apply or paste onto the other side (left and right swapped) when you double-click or drag.
- **Right-click** an item for **Apply at This Frame** (or **Paste at This Frame**), **Apply Mirrored** (or **Paste Mirrored**), **Rename...** and **Delete**.

Applying or pasting is one undo step. If some of a clip's bones don't fit, a **Clip pasted** message lists what was left out.

### Starter poses

The **Starter poses** section lists the poses that ship with VATs, each marked **hand** or **body**.

| Kind | Poses |
|---|---|
| Hand | Relaxed, Rest, Open (Spread), Flat, Fist, Loose Fist, Point, Point (Thumb Up), Peace (V), Thumbs Up, OK, Pinch, Pinch (Loose), Grip (Cylinder), Hold Glass (Stem), Hold Phone, Cup (C-Shape), Claw, Rock (Horns), Call Me, Pistol (Finger Gun), Salute, Wave, Typing, Resting on Surface, Holding Pen, Counting 1 to Counting 5 |
| Body | Relaxed Stand, Hands on Hips, Arms Crossed, Thinking, Waving, Sitting, Contrapposto |

- **Click** a hand pose to put it on the left hand, **Shift+click** for the right hand.
- **Click** a body pose to apply it at the current frame.
- **Drag** a pose onto the view to apply it.
- **Right-click** a hand pose for **Left Hand**, **Right Hand** or **Both Hands**; right-click a body pose for **Apply at This Frame**.

Starter poses can't be renamed or deleted.

## Configuration

The library is one file, `poses.json`, in the `library` folder of VATs' data folder:

| System | Path |
|---|---|
| Linux | `~/.local/share/viewport-avatar-toolset/library/poses.json` |
| Windows | `%APPDATA%\viewport-avatar-toolset\library\poses.json` |

Starting VATs with `--data-dir` moves everything, the library included, to that folder. See [[Preferences]].

## Tips and tricks

- Save a set of hand shapes you use often, then adjust them per shot with the [[Hand poser]].
- A saved clip of the head and neck makes a reusable nod or glance; paste it mirrored for the other direction.
- Save a pose of only the selected bones to layer it over different body poses.

## Troubleshooting

### "Pose library damaged"

VATs could not read `poses.json`. It renames the file to `poses.json.corrupt-` followed by a number and starts an empty library, so the damaged file is kept and not overwritten. Restore a backup, or open the renamed file to recover what you can.

### Deleted a pose by mistake

Deleting asks first: "Delete "*name*" from the Inventory? This cannot be undone." Once confirmed, it is gone from `poses.json`; **Edit → Undo** does not bring it back.

## App and viewer

> **Note:** In the viewer the **Inventory** shows plain icons instead of pose and prop thumbnails.

## See also

- [[Hand poser]]
- [[Projects and files]]

Category: Animating
