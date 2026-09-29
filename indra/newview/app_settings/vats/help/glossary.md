# Glossary

The terms VATs and this help use, with a short definition each and a link to the page that covers the
topic. Second Life names are given as Second Life uses them.

> Related articles: [[VATs]], [[Skeleton]], [[Keys and timeline]]

## Terms

- **.anim**: Second Life's native animation format, the file an upload sends. See [[Anim format]].
- **Actor**: One animated avatar in a project with several. Each actor has its own animation and exports
  its own file. See [[Couples and groups]].
- **Animation priority**: A number from 0 to 6 that decides which animation moves a joint when several
  play at once; the higher priority wins. See [[Animation priority]].
- **Attachment point**: A named point on the avatar, such as `Right Hand` or `Chest`, where worn objects
  sit. VATs can animate and pin attachment points. See [[Skeleton]].
- **Bake**: To turn a simulation or a rig result into ordinary keys, because Second Life plays keys, not
  physics. See [[Dynamics]] and [[Ragdoll]].
- **Bento**: Second Life's extended skeleton, with face, finger, tail, wing, hind-limb and groin bones.
  See [[Skeleton]].
- **Bind**: A pin that makes a point follow another bone. See [[Hold and bind]].
- **Blendshape**: A named face movement, such as `jawOpen`, with a weight from 0 to 1, sent by
  face-tracking apps. VATs turns blendshapes into face-bone motion. See [[Face tracking]].
- **Bone, joint**: One node of the skeleton, such as `mShoulderLeft`. This help uses the two words for
  the same thing.
- **BVH**: A text motion-capture format that many tools read and write. See [[BVH]].
- **Channel**: One animated value of a bone: `rot_x`, `rot_y`, `rot_z`, `pos_x`, `pos_y` or `pos_z`. Each
  channel is one curve. See [[Graph editor]].
- **Collision volume**: A body-shape volume of the skeleton, such as `BELLY` or `LEFT_PEC`. Mesh bodies
  use them for soft parts, and [[Dynamics]] uses them for jiggle. Shown with **View → Show Collision
  Volumes**.
- **Curve**: The values of one channel over time, drawn through its keys. See [[Graph editor]].
- **Deformer**: An animation whose position keys reshape the avatar (a long neck, a taller body) and stay on after
  it stops; an *undeformer* puts the bones back. See [[Deformers]].
- **Devkit**: The files a mesh-body maker gives creators, used to animate against that body's shape. See
  [[Mesh bodies]].
- **Ease in, ease out**: The time, in seconds, Second Life takes to blend an animation in when it starts
  and out when it stops. See [[Export to Second Life]].
- **Emote**: One of Second Life's built-in facial expressions, played with an animation. See
  [[Export to Second Life]].
- **FK**: Forward kinematics: posing by rotating each bone from the body outwards. The opposite of
  [[IK]].
- **Frame**: One step of the timeline. The frame rate (`fps`) sets how many frames make a second. See
  [[Keys and timeline]].
- **Ghost**: A faded copy of the pose at another frame, drawn by the [[Onion skin]].
- **Hand pose**: One of Second Life's 14 built-in hand shapes (0–13), played with an animation. See
  [[Hand poser]].
- **Handle, tangent**: The controls on each side of a key that shape the curve through it. See
  [[Graph editor#Shaping curves: tangents]].
- **Hold**: A pin that keeps a point still in the world. See [[Hold and bind]].
- **IK**: Inverse kinematics: posing a limb by moving its end, such as a hand or foot, and letting the
  joints follow. See [[IK]].
- **Key**: A stored value of a channel at a frame. VATs interpolates between keys. See
  [[Keys and timeline]].
- **Loop points**: The frames where a looping animation starts and ends its repeat. See [[Loop tools]].
- **Motion capture, mocap**: Recording motion from a tracker or suit instead of keying it by hand. See
  [[Motion capture]].
- **Neutral face**: The relaxed expression that face tracking treats as rest. See
  [[Face tracking#Setting the neutral face]].
- **Pin**: A hold or a bind: a point kept in place in the world, or following another bone, over a range
  of frames. See [[Hold and bind]].
- **Pole**: The point an IK limb's elbow or knee aims at. See [[IK]].
- **Prop**: An object, such as a cup or a chair, loaded from a `.dae` file to animate against. Props are
  not part of the exported animation. See [[Props]].
- **Punch in**: Recording a take into a set frame range, leaving keys outside it alone. See
  [[Motion capture#Recording a take]].
- **Rest pose**: The skeleton's pose with no animation. For motion capture, the pose the sender's joints
  have at rest. See [[Skeleton]].
- **Retargeting**: Transferring motion from another skeleton, such as a BVH, glTF or FBX rig, onto the SL
  skeleton. See [[Retargeting]].
- **Scrub**: To drag the playhead through the timeline to see the animation at each frame.
- **SoapStorm**: The Second Life viewer that includes the [[VATs Editor (viewer)|VATs Editor]].
- **.vat**: VATs' project file. See [[Project file format]].
- **Take**: One recording of motion capture, kept as one undo step. See [[Motion capture]].
- **T-pose**: A standing pose with the arms straight out to the sides. Motion-capture senders use it as
  their rest pose.
- **VMC protocol**: A network protocol that webcam and VR tracking apps use to send avatar motion. See
  [[Motion capture]].

## See also

- [[Keyboard shortcuts]]
- [[Troubleshooting]]

Category: Reference
