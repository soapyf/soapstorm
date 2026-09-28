# Mesh bodies

A mesh body is a rigged avatar body from a developer kit ("devkit"), used in Second Life in place of the
Linden body. VATs can show your mesh body in the view and use its joint positions, so IK, pins and the
exported animation fit its proportions. VATs remembers where the devkit files are and never copies,
shares or uploads them.

> Related articles: [[Props]], [[Export to Second Life]], [[IK]], [[Hold and bind]], [[Skeleton]]

> **Note:** In the [[VATs Editor (viewer)]] the mesh body you wear is your own body, and its joint
> positions come from your avatar. A body imported here is for the other actors of a
> [[Couples and groups|couple or group]], chosen as their **Body** in the **Actors** window.

## Usage

### Import a body

![The Bodies section of the Inventory with only the Linden body and the Import Body Parts button](images/mesh-bodies/bodies.png)
*Before any import: **Linden body** is the only entry, highlighted as the body shown.*

1. Open the **Inventory** and find **Bodies** at the top.
2. Press **Import Body Parts (.dae, .fbx)...** and choose every part at once: body, head, hands and
   feet.
3. VATs checks each part and reports the result in **Imported body** with the parts, their triangle
   counts and any weights to joints it does not know. Parts that cannot be read, or that are not rigged
   to the SL skeleton, are listed under **Left out**.

The body takes the name of the first file and becomes the body shown. An import where no part is usable
reports **No body imported**.

Importing a whole-body mesh with **File → Import Prop / Mesh (.dae, .fbx)...** also works: VATs sees
that it is rigged to most of the skeleton and asks **This Looks Like an Avatar Body**. Choose **Use as
Body**, **Add as Prop** or **Cancel**.

### Switch bodies

- In **Inventory → Bodies**, double-click a body, or **Linden body** to go back. Hover a body to see its
  files.
- **View → Body** lists the Linden shapes (**SL Default**, **SL Default (Male)**, **Female**, **Male**,
  **Skeleton Only**) and, under **Mesh bodies**, your bodies.
- Right-click a body for **Use** or **Remove from Inventory**. Removing it does not delete the mesh
  files.

The body shown is a preference, not part of the project.

### Pose on the body's proportions

While a mesh body is shown, the joints and collision volumes sit where the devkit's binds put them. A
devkit with longer legs poses with longer legs, and IK and pins reach the positions the body really
has.

What the view shows does not change the export. To bake IK and pins against the body, set
**Properties → Export → Bake shape** to **Mesh body:** and the body's name; see
[[Export to Second Life#Choose the bake shape]].

### Export a devkit from Blender

Select the armature and every mesh part, then export FBX with the default settings and **Add Leaf Bones**
off. Import all the parts together, so they share one alignment.

## How VATs reads a devkit

- **Axes.** A devkit exported with other axes is turned upright, and a quarter turn about the vertical
  is applied when the binds clearly call for it. All parts of one body share the same turn, so the eyes
  and head stay on the body.
- **Bone-oriented binds.** FBX files whose binds follow Blender's bone orientation are aligned joint by
  joint.
- **Mixed COLLADA.** Blender's COLLADA exporter can write SL bind data for some joints and Blender's own
  positions for others, often the face. VATs corrects each such joint on its own.
- **Weights.** A vertex with more than four weights keeps its strongest four, as in Second Life.

## Troubleshooting

### A part is left out: not rigged to the SL skeleton

The file has no skin, or its joints are not SL bones. Export the part again with the armature selected.

### Face bones on the centre line are slightly off

In COLLADA files, a joint within 1 cm of the centre line, such as `mFaceRoot`, cannot be told apart
from a correctly placed one and stays as written. Export the devkit as FBX instead.

### The body is skinny or wide in places compared to Second Life

The fitted-mesh shape sliders that scale the collision volumes in-world are not modelled; the body is
shown at the devkit's own proportions.

### A body added as a prop does not change the proportions

Only a body in **Inventory → Bodies** shapes the avatar. A body added as a prop follows the Linden
proportions.

### Weights to an unknown joint

The report lists joints the devkit weights to that are not in the SL skeleton. Those vertices do not
move with the animation.

Category: Import and export
