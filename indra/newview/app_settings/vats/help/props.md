# Props

Props are meshes shown in the scene with the avatar: a cup in the hand, a chair to sit on, a hat, or a
rigged garment. They help you pose against real objects and line up an animation with the furniture it
will play on. Props are saved in the project but are not part of the exported animation.

> Related articles: [[Mesh bodies]], [[Pose library]], [[Projects and files]], [[Export to Second Life]]

> **Note:** The [[VATs Editor (viewer)]] has no imported props. In the viewer, the objects you wear
> and the objects rezzed in-world are the props, and attachment-point keys move what you wear.

## Usage

### Import a mesh

Choose **File → Import Prop / Mesh (.dae, .fbx)...** (**Ctrl+I**; **Ctrl+Alt+I** in the Blender preset),
or press **Import .dae / .fbx...** under **Inventory → Meshes**. VATs reads COLLADA (`.dae`) and FBX
files, adds the mesh to the scene, and adds it to the Inventory so you can use it in other projects.

A message reports the triangle count, whether the mesh is **rigged** or **static**, the scale and up
axis, and anything it skipped: joints that are not SL bones, missing textures and unsupported features.

- A **static** mesh has its own position, rotation and scale, and can be attached to a bone or an
  attachment point.
- A **rigged** mesh follows the avatar and has no transform of its own.

If a rigged mesh covers most of the skeleton, VATs asks whether it is an avatar body; see
[[Mesh bodies]].

### Add a prop from the Inventory

**Inventory → Meshes** shows your imported meshes as a grid of thumbnails, followed by **Starter props**
grouped by category. Hover a thumbnail to see its parent, category and file.

- **Double-click** adds the prop at its usual place: the parent and offset it was saved with.
- **Drag** it onto a bone or attachment point in the view to attach it there, or onto empty space to
  place it in the world. A note by the cursor says which: `Attach ... to ...` or `Place ... in the
  world`. A starter prop made for a hand snaps to its grip when you drop it anywhere on that hand,
  fingers included.
- **Right-click** for more:
  - **Add to Scene**, the same as double-click;
  - **Attach to Point**, a submenu of every attachment point;
  - **Attach to** the selected bone (**Attach to Selected Bone (select one first)** when none is
    selected);
  - **Save As Variant**, a new Inventory item from the selected prop in the scene, which must use the
    same mesh;
  - **Save (from Selected Prop)**, **Rename...** and **Delete from Inventory**, for your own items only.

Starter props cannot be renamed, saved over or deleted.

### Place a static prop

Click a prop in the view to select it. **Properties → Prop** shows its name and:

| Field | Meaning |
|---|---|
| **Parent** | **World**, an attachment point, or a bone. Changing it snaps the prop to the new parent. |
| **Position** | Metres, relative to the parent. |
| **Rotation** | Degrees, relative to the parent. |
| **Scale** | A factor on each axis. |
| **Visible** | Hides the prop without removing it. |

A prop is positioned by the centre of its bounding box, not by the mesh's own origin. Each change is one
undo step. **Remove Prop** takes it out of the scene; the Inventory item stays.

### Copy values to and from Second Life

To build the real object in-world with the same placement, use **Properties → Prop → Copy for SL** and
press **Position**, **Rotation** or **Size**. VATs copies an SL vector such as `<0.12000, 0.00000,
0.45000>` to paste into the SL build window.

With a static prop selected, **Ctrl+C** opens a small menu with the same three choices. **Ctrl+V** reads
an SL vector from the clipboard and asks whether to paste it as **Position**, **Rotation** or **Size**.
Pasting a size sets the scale so the prop has that size.

## Tips and tricks

- Use a chair or bed prop with [[Couples and groups]] to check where each actor sits.
- Save a hand-held prop with the right grip as a variant, so each new project gets it in place with one
  double-click.

## Troubleshooting

### Some prop meshes are missing

The project refers to a mesh file that has moved or been deleted. The prop shows as an orange box and
**Properties → Prop** says `Mesh file not found:` with the path. Put the file back at that path, or remove
the prop and import the mesh again. The project still opens and saves.

### Paste needs an SL vector like `<1.0, 2.0, 3.0>` on the clipboard

The clipboard does not hold a vector. Copy the value from the SL build window, including the angle
brackets.

### A prop does not appear in the animation in-world

Props are not written to the `.anim`. Rez or wear the real object in Second Life.

### Could not add: The mesh file is missing or could not be read

The Inventory item's mesh has moved. Delete the item and import the mesh again.

Category: Import and export
