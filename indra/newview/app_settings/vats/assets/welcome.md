# What's new in 0.2.0

## Posing
- **Drag the body itself.** Press any part of the avatar and pull: the bones above it follow (Auto IK), and dragging the hips or chest keeps the feet planted. **Tools > Auto IK**.
- **Joint limits.** Knees and elbows stop where a body stops, in the gizmo, IK and body drags. **Tools > Respect Joint Limits**; **Rig > Suggest Joint Limits...** fits limits to your body.
- **Sit on This** puts the avatar onto a chair or sofa prop in one step, feet held on the floor. **Properties > Prop** or **Tools > Sit on Seat**.
- **Flip Animation** swaps left and right on every key: animate one hand, flip it for the other. **Edit** menu.

## Rigging your own models
- **Map Rig to Second Life**: a game character, creature or mech rigged to bones of its own goes onto SL's skeleton, with spare chains for scarves and tails. **Rig** menu.
- **Rig a Model from Scratch**: drag markers onto the joints of a model with no skeleton and VATs weights it; **Paint Weights** touches up any rigged body with a brush: click a bone to pick it, drag on the body to paint. **Rig** menu.
- **Export Rigged Mesh for SL** writes a rigged .dae the uploader takes, checked against its rules first, and the **Joint Offset Inspector** shows which joint positions will upload. **File** and **Rig** menus.
- **Parts and shape keys**: hide a model's clothes and set its shape keys; the rigged export writes what you see. **Properties > Body**; the parts are also in **View > Body**.
- **Deformers** that stay on without sinking the wearer, with an undeformer to put the bones back. Export settings, **Clean Up > Deformer**.

## Faces
- **Face poses from shape keys**: right-click a face key's slider under **Shape Keys** to turn a viseme or expression into a Bento face pose SL can play.
- **Smiles, frowns and puckers** move the face bones, sized for the head the export plays on, everywhere from face tracking to lip sync. **Move face bones** in **Tools > Face...**.

## Interface
- **Workspaces**: **Pose**, **Animate**, **Face**, **Rig** and **Export** tabs in the menu bar, each with only the panels that job needs. **View > Workspaces**.
- **The Tab pie**: hold **Tab** over the view and flick toward a tool.
- **Find a Tool** (**F3**), and a search box at the top of every menu that finds any command by name, submenus included.
- **Undo History**: every undo step by name; click one to go back or forward to it. **Edit > Undo History**.
- In SoapStorm: **Viewer > Inventory** opens the viewer's own Inventory inside the editor, to wear and take off objects.
- New installations start with the **Second Life** controls. **Edit > Preferences... > Navigation & hotkeys** has the others.

## Help
- **The beginner tutorials are rewritten** to better reflect user experience. Some images and examples continue to use the old bone examples and will be replaced in a future revision. **First Steps** and **Tutorials** above.

# What's new in 0.1.1

- **Creatures import at their real size.** A rigged mesh with its own proportions (a creature, a mech) keeps the unit its file declares, as Second Life does, instead of being shrunk to fit.
- **In SoapStorm: another body in your avatar's place.** View > Body shows a mesh body where you stand, on your screen only, posed on its own joints.

# What's new in 0.1.0

## Posing
- **IK** for arms, legs, wings, hind legs, fingers and the spine. Press K on a limb to switch between IK and FK without a jump.
- **Hold and bind**: hold a point still in the world, or bind it to another bone so it follows (set a drink down, pass it to the other hand, a two-handed weapon).
- **Hand poser** (H): drag a finger dot down to curl it, sideways to spread it. Starter hand and body poses are in the Inventory.
- **Your own mesh body**: import a devkit's rigged parts under Inventory > Bodies.

## Animating
- **Graph editor** (Ctrl+G) with every tangent type, box select, scaling and an Euler filter.
- **Poses and clips** in the Inventory, pasted mirrored onto the other side when you need them.
- Mirror, flip and reverse whole animations.
- **Couples and groups** (Tools > Actors): several avatars on one timeline, one .anim each.

## Bringing motion in
- **Retargeting** (File > Import Animation (Retarget)): Mixamo, Unreal, VRM, Rigify and mocap files as BVH, glTF or FBX, fitted under SL's limits.
- **Motion capture** (Tools > Motion Capture): live from webcam and VR tracker apps that send VMC.

## Making motion
- **Dynamics** (Tools > Dynamics): tails, ears, hair and jiggle that swing behind the animation, baked to keys.
- **Ragdoll** (Tools > Ragdoll): let the body or a limb fall limp, then bake it.

## Exporting
- **.anim files the viewer reads exactly** as it reads its own, checked against the viewer's own rules before they are written.
- BVH export and import for other tools.
- Naming patterns and an export folder in the Export section, so exporting takes one click.

## Where your work lives
- Projects are plain .vat files.
- Settings, autosaves and your libraries are kept in your user folder.
