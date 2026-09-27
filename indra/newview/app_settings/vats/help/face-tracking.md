# Face tracking

Face tracking turns the blendshapes a tracking app sends into motion of the Bento face bones: jaw, lips,
eyelids, brows, cheeks and tongue, plus the eyes. It is part of [[Motion capture]] and works with
iFacialMocap on an iPhone and with VMC apps that send blendshapes.

> Related articles: [[Motion capture]], [[Skeleton]], [[Mesh bodies]]

## Usage

The face settings are in the **Face** section of **Tools → Motion Capture...**. **Use Face Tracking** is
on by default. **Shapes** shows how many blendshapes the sender sends, or **none received**.

> **Note:** Face bones only show in Second Life on a mesh head rigged to the Bento face bones. On a
> system head or an unrigged head, a face take has no visible effect.

### Connecting iFacialMocap

1. Install iFacialMocap on an iPhone or iPad with Face ID, on the same Wi-Fi as the computer.
2. Set **Source** to **iFacialMocap (iPhone)**. The port changes to `49983` and **Allow Other Devices**
   is on.
3. Click **Listen**.
4. Type the address the iFacialMocap app shows into **Phone address**.
5. Press **Connect to iPhone**. The status bar shows **Asked the iPhone to start streaming**, and the app
   starts sending to this computer.

iFacialMocap sends the face and the head. **Head from iPhone** (on by default) keys the head's turn from
its head tracking; untick it to keep the head from another take.

### VMC apps

VMC apps that send blendshapes, such as VSeeFace with a perfect-sync model, need no extra step. ARKit
names are used directly; the VRM names (`a`, `i`, `u`, `e`, `o` and the other VRM expressions) are
converted to ARKit shapes first.

### Setting the neutral face

Relax your face and press **Capture Neutral Face**. Your resting expression becomes the rest pose: each
shape then counts only from its resting value, so a face that rests with the mouth slightly open does not
record an open jaw. **Clear** drops the neutral face.

### Recording the face alone

Tick **Face Only** in the **Record** section to record only the face bones (and the head from an iPhone)
over the animation already there. See [[Motion capture#Recording a take]].

## Configuration

| Setting | Range | Default | Effect |
|---|---|---|---|
| **Preset** | Natural, Subtle, Expressive | Natural | sets **Strength** and the shape strengths together |
| **Strength** | 0–2 | 1 | scales every shape |
| **Eye Strength** | 0–2 | 1 | how far the eyes follow the tracked gaze |
| **Eye Limit** Side | 5–45° | 25° | the farthest the eyes turn left or right |
| **Eye Limit** Up/Down | 5–45° | 20° | the farthest the eyes turn up or down |
| **Shape Strengths** | 0–2 per shape | 1 | scales one shape, for example `jawOpen` |

The presets: **Natural** is strength 1; **Subtle** is 0.6; **Expressive** is 1.35, with `jawOpen` held at
1.1. After strengths are applied, each shape weight is limited to 0–1.5.

The face settings, the neutral face included, are saved in `settings.json` and come back at the next
start.

### Eyes and eyelids

The eyes turn `mEyeLeft`/`mEyeRight` and `mFaceEyeAltLeft`/`mFaceEyeAltRight` together. The eye angles
come from the sender when it sends them (the iFacialMocap eye values, or the VMC `LeftEye` and `RightEye`
bones); otherwise they come from the `eyeLook` shapes. The eyelids follow the eyes up and down: the upper
lid follows 40 % of the eye's pitch and the lower lid 15 %.

### The face table

The mapping from the 52 ARKit shapes to face-bone rotations (degrees) and offsets (metres) is the data
file `data/retarget/face-arkit.json`, made for the SL default head. The right side mirrors the left. The
presets, the VRM names and the eyelid fractions are in the same file. VATs reads it when the Motion
Capture window first opens.

## Tips and tricks

- Capture the neutral face again whenever you move the phone or change the light.
- Lower **Eye Limit** when a mesh head's eyes show white at the edges.
- To tame one shape, for example a jaw that opens too far, lower it in **Shape Strengths** instead of
  lowering **Strength**.

## Troubleshooting

### Shapes shows none received

The sender sends no blendshapes. For a VMC app, use a model with blendshapes and switch on the app's
blendshape sending. Rokoko face data is ignored.

### The iPhone does not start sending

**Connect to iPhone** needs VATs to be listening and a **Phone address**. Check the address the app
shows, the same Wi-Fi on both devices and the firewall line of the setup checklist (see
[[Motion capture#The setup checklist]]).

### The face looks tense at rest

The resting face records as an expression. Press **Capture Neutral Face** with a relaxed face.

### The window shows "data/retarget/face-arkit.json is missing"

The face table was not found in VATs' data folder. Reinstall VATs (see [[Installation]]).

## See also

- [[Motion capture]]
- [[VATs Animator (viewer)#Motion capture]]
- [ARKit blend shape locations](https://developer.apple.com/documentation/arkit/arfaceanchor/blendshapelocation)

Category: Motion
