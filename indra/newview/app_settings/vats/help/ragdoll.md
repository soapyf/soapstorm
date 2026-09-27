# Ragdoll

The ragdoll lets the whole body, or some limbs, fall limp under gravity from a chosen frame. It collides
with the ground and with props. VATs simulates the fall, lets you scrub it, and bakes it into ordinary
keys.

> Related articles: [[Dynamics]], [[Props]], [[IK]], [[Keys and timeline]]

## Usage

Open the window with **Tools → Ragdoll...**. A clip has at most one ragdoll.

### Setting up a fall

1. Move the playhead to the frame where the fall begins.
2. Click **Set Up Ragdoll**. The fall starts at the current frame and lasts 60 frames, or up to the last
   frame if that is sooner.
3. Choose **Whole body**, or **Selected bones**. For selected bones, select them in the view or the bone
   list, then click **Use the Selected Bones**. Those bones and every ragdoll joint below them fall limp;
   the rest keeps its animation.
4. Set the frames and the body settings (see Configuration below).
5. Click **Simulate**. The fall shows in the view as **Showing the simulated preview**, and you can scrub
   through it. No keys change.
6. Click **Bake** to write the fall as keys, as one undo step.

### Re-baking and clearing

- **Re-bake** starts again from the keys the clip had before the first bake.
- **Clear** puts back the keys from before the bake and removes the ragdoll.

### Which joints fall

The ragdoll moves the pelvis, torso, chest, neck and head, and on each side the shoulder, elbow, wrist,
hip, knee and ankle. Fingers, face, tail, wings and the other Bento bones keep their animation.

## Configuration

### Frames

| Setting | Range | Default | Effect |
|---|---|---|---|
| **Start** | 0 to the last frame | the playhead at setup | the frame the ragdoll takes over from the animation; **Current** uses the playhead |
| **Length** | 1 to the last frame | 60 | how many frames it falls for |
| **Blend in** | 0–60 frames | 3 | frames to ease from the animation into the fall |
| **Blend out** | 0–60 frames | 0 | frames to ease back to the animation at the end; 0 stays down |

### Body

| Setting | Range | Default | Effect |
|---|---|---|---|
| **Gravity** | 0–2 g | 1.00 g | pull downwards, in multiples of Earth's gravity |
| **Stiffness** | 0–1 | 0 | how hard joints pull towards the animated pose; 0 is limp |
| **Friction** | 0–1 | 0.6 | grip on the ground and on props |
| **Fall direction** | Forward, Back, Left, Right, Random, None | Forward | which way a standing body topples when it goes limp |

**Fall direction** only nudges a standing body whose pelvis falls. A seated, kneeling or lying body
slumps on its own. **Random** is repeatable: the same start frame gives the same fall.

The simulation runs at 480 sub-steps per second. The ground is at height 0. Visible props that are not
rigged collide as solid boxes, sized to their bounds.

## Tips and tricks

- For a limp arm on an otherwise animated body, use **Selected bones** with the shoulder joint selected.
- Raise **Stiffness** a little for a collapse that keeps some muscle, for example a faint rather than a
  dead drop.
- Use **Blend out** to get back up: the body eases from where it landed back into the animation.

> **Note:** In the viewer the ragdoll previews on your avatar and lands on the ground only; in-world
> objects are not in its way.

## Troubleshooting

### The window warns "Uses IK in this range"

Limbs that are in [[IK]] during the fall keep following their IK targets, so their baked keys do not show
there. Switch the named limbs to FK first.

### The body lands on air above a prop

Props collide as boxes up to their bounds, so a chair is a block up to the top of its backrest. Rigged
props and hidden props do not collide. Place the fall so the body lands where the box matches the prop,
or hide the prop before you simulate.

### Simulating takes a while

A long fall takes a moment to simulate. Shorten **Length**.

## See also

- [[Dynamics]]
- [[Props]]
- [[Project file format#ragdoll]]

Category: Motion
