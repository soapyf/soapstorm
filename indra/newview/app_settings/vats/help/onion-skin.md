# Onion skin

Onion skinning draws faint ghosts of the pose a few frames before and after the current frame, so you can judge arcs and spacing without scrubbing back and forth. Earlier ghosts are cool (blue) and later ones warm (orange); nearer ghosts are stronger.

> Related articles: [[Keys and timeline]], [[Graph editor]], [[Posing]]

## Usage

### Showing ghosts

Tick **View → Onion Skin → Show Ghosts**. The ghosts appear around the body at the current frame and follow the playhead as you scrub.

Ghosts are see-through and can't be clicked, so they never get in the way of selecting bones. They hide while the animation plays.

## Configuration

All settings are in **View → Onion Skin**:

| Setting | Values | Default | What it does |
|---|---|---|---|
| **Show Ghosts** | on / off | off | Draws the ghosts |
| **Before** | 0–5 | 2 | Ghosts before the current frame |
| **After** | 0–5 | 2 | Ghosts after the current frame |
| **Keyed Frames Only** | on / off | off | Puts the ghosts on keyed frames instead of evenly spaced frames |
| **Every** | 1–10 frames | 1 | Frames between ghosts; greyed out with **Keyed Frames Only** |
| **Bones Only** | on / off | off | Draws the ghosts as bones instead of the body |

With **Keyed Frames Only**, the ghosts go on the nearest frames that hold a key on any bone. Ghosts stop at frame 0 and at the last frame; they don't wrap round a loop.

The settings are saved with the project, so each project remembers its own.

## Tips and tricks

- For a walk or run, set **Every** to the number of frames between contact poses to line up the steps.
- **Keyed Frames Only** shows your poses and nothing in between, which suits blocking.
- Turn on **Bones Only** when the ghost bodies overlap too much to read, for example on a pose that hardly moves.
- With **View → Body → Skeleton Only**, the ghosts are always drawn as bones.
- With a mesh body, the ghosts are drawn as that body.

## Troubleshooting

### No ghosts appear

Check that **Show Ghosts** is ticked, that playback is stopped, and that **Before** and **After** are not both 0. At frame 0 there are no earlier ghosts, and at the last frame there are no later ones. With **Keyed Frames Only**, an animation with only one key has nothing to show.

### Ghosts look the same as the body

The pose doesn't change around this frame. Raise **Every** to spread the ghosts further apart, or use **Keyed Frames Only**.

## App and viewer

> **Note:** Onion skin is not yet available in the VATs Animator in the viewer.

## See also

- [[Keys and timeline]]
- [[Loop tools]]

Category: Animating
