# Motion capture

Motion capture records live body and face motion from a tracking app or suit into the animation. VATs
receives the VMC protocol, Rokoko Studio Live, and iPhone face tracking from iFacialMocap, VTube Studio
and Live Link Face over UDP on your local network. It shows the motion on the avatar while it arrives,
and records takes as keys.

> Related articles: [[Face tracking]], [[Retargeting]], [[Keys and timeline]], [[VATs Editor (viewer)]]

## Usage

Open the window with **Tools → Motion Capture...**. It starts with **Listen** and a line saying what arrives,
then **Source** (its tooltip says what the source is and how to point it here), then the **Setup** checklist, which
is open while nothing arrives and folds itself once data flows, then the **Live**, **Face**, **Record**,
**Clean-up** and **Last take** sections. **Setup** holds **Your computer**, the checklist, **Port** and **Allow
other devices**.

![The Motion Capture window before listening: the Setup checklist, the Connection row with Source, Port and Listen, and the Live section](images/motion-capture/setup-and-connection.png)
*Before **Listen**: a grey dot marks what is still to do, a green one what is done. The dot after **Listen** turns amber while VATs waits for a sender and green once packets arrive.*

### Sources

| Source | Default port | Allow other devices | Sent by |
|---|---|---|---|
| **VMC protocol** | `39539` | off | webcam and VR-tracker apps, for example XR Animator, VSeeFace and VirtualMotionCapture |
| **Rokoko Studio Live** | `14043` | off | Rokoko suits through Rokoko Studio |
| **iFacialMocap (iPhone)** | `49983` | on | the iFacialMocap app on an iPhone or iPad with Face ID |
| **VTube Studio (iPhone)** | `21413` | on | the VTube Studio app on an iPhone or iPad with Face ID |
| **Live Link Face (iPhone)** | `11111` | on | the Live Link Face app on an iPhone or iPad with Face ID |

The three iPhone sources always run on another device, so **Allow other devices** stays on for them.

Choosing a source sets its default port. A port you typed yourself is kept when you switch source.

### Connecting a VMC app

1. Keep **Source** on **VMC protocol** and **Port** on `39539`.
2. If the tracking app runs on another device, tick **Allow other devices**. Off, only apps on this
   computer can send.
3. Click **Listen**.
4. In the tracking app, switch on its VMC sender and point it at the address shown under **Your
   computer** and the same port.

VMC apps send a VRM-style avatar that stands in a T-pose at rest. If the arms or legs come in twisted,
stand in a T-pose and press **Capture Rest Pose Now**. **Reset to T-Pose** drops the captured rest pose.

### Apps that work with VATs

iFacialMocap has been checked with a real iPhone. The steps for the others come from their own settings
and documentation, and have not been checked with VATs on real hardware yet.

| App | Tracks | Runs on | Price | VATs **Source** |
|---|---|---|---|---|
| **SlimeVR Server** | body, from SlimeVR trackers | Windows, Linux | free (trackers are bought or built) | **VMC protocol** |
| **XR Animator** | body, hands and face from a webcam | Windows, Linux, macOS, browser | free | **VMC protocol** |
| **VirtualMotionCapture** | body from SteamVR trackers, Kinect (through Amethyst) or mocopi; Index fingers | Windows | free | **VMC protocol** |
| **VSeeFace** with Leap Motion | face from a webcam, hands and fingers from a Leap Motion | Windows | free | **VMC protocol** |
| **Waidayo** | face and head | iPhone or iPad with Face ID | free | **VMC protocol** |
| **AndroidMoCap** | face (52 ARKit shapes) and gaze | Android 11 or later | free | **VMC protocol** or **iFacialMocap (iPhone)** |
| **MeowFace** | face and head | Android (unmaintained) | pay what you want | **iFacialMocap (iPhone)** |
| **iFacialMocap** | face, head and eyes | iPhone or iPad with Face ID | paid | **iFacialMocap (iPhone)** |
| **VTube Studio** | face, head and eyes | iPhone or iPad with Face ID | free | **VTube Studio (iPhone)** |
| **Live Link Face** | face, head and eyes | iPhone or iPad with Face ID | free | **Live Link Face (iPhone)** |
| **Rokoko Face Capture** | face, through Rokoko Studio | iPhone with Face ID | Rokoko Studio Pro plan | **Rokoko Studio Live** |

For a sender on another device, tick **Allow other devices** and enter the address under **Your
computer** in the app. For an app on this computer, enter `127.0.0.1`.

**SlimeVR Server**

1. In VATs, keep **Source** on **VMC protocol**, port `39539`, and click **Listen**.
2. In SlimeVR Server, open **Settings → OSC → Virtual Motion Capture** and turn on **Enable**.
3. Leave the output port at `39539`, and the address at `127.0.0.1` when both run on this computer.
4. For walking takes, untick **Anchor at hips**. It is on by default and pins the hips in place, so
   the hips never travel.

SlimeVR sends the hips, spine, chest, neck, head, legs, feet and arms, as its trackers cover them. It
sends no blendshapes, toes, eyes or jaw. Fingers come only with finger trackers, and the arms are left
out when controllers track them.

**XR Animator** runs as a desktop app on Linux, Windows and macOS (and in a browser). Turn on its VMC
protocol output with port `39539` and choose **VMC protocol** in VATs.

**VirtualMotionCapture** (Windows) turns SteamVR into VMC: a headset and controllers, Vive or SlimeVR
trackers, Kinect through Amethyst (which shows a Kinect as SteamVR trackers), Index finger tracking and
Sony mocopi. Turn on its VMC protocol sending with this computer's address and port `39539`.

**VSeeFace with Leap Motion** (Windows) is the way to capture fingers without gloves. Turn on hand
tracking with the Leap Motion in VSeeFace, then its VMC protocol sender with this computer's address and
port `39539`. The hand and finger bones arrive with the rest of the body.

For the face apps, see [[Face tracking]].

### Connecting Rokoko Studio

1. Set **Source** to **Rokoko Studio Live**. The port changes to `14043`.
2. In Rokoko Studio, add a **Custom** streaming target with this computer's address, the same port and
   the **JSON v3** data format.
3. Click **Listen**. The actor name appears once data arrives.
4. Stand in a T-pose and press **Capture Rest Pose Now**. Rokoko's joints have their own rest directions,
   so do this every time you connect.

VATs takes the first actor in the stream that has a body, or else the first with a face. An actor's face
from Rokoko Face Capture arrives with it; see [[Face tracking#Rokoko Face Capture]]. Glove data is
ignored.

### Connecting an iPhone

See [[Face tracking#Connecting iFacialMocap]], [[Face tracking#Connecting VTube Studio]] and
[[Face tracking#Connecting Live Link Face]]. The iPhone apps send the face and head only. The window
listens to one source at a time, so record the body in a separate take with a body source.

### The setup checklist

The checklist shows what still stands between the sender and VATs:

- **Your computer**: this computer's address, with **Copy**. Enter it in the sending app, on the same
  Wi-Fi.
- **Listening on port N**, or **Not listening yet: press Listen above.**
- **Other devices can send**, or a button **Allow Other Devices and Listen Again**.
- The firewall state: checking, none found, open, unknown, or probably blocking. When a Linux firewall is
  probably blocking the port, **Allow on My Home Network** opens it for your local subnet only (your
  system asks for your password), and **Copy Command** copies the command to type yourself. On Windows,
  allow VATs under **Windows Security → Firewall & network protection → Allow an app through firewall**;
  on macOS, under **System Settings → Network → Firewall → Options**.
- **Receiving N packets/s from** the sender, or **No data for N s**.

### Watching the motion

In the **Live** section, **Drive the avatar** (on by default) shows the incoming motion on the avatar
while listening. The clip does not change until you record.

### Recording a take

![The Record and Clean-up sections with their defaults](images/motion-capture/record-and-clean-up.png)
*The defaults: a take starts at frame 0 after a 3 s countdown, keys are reduced within 0.5° and 2 mm, and the edges blend over 4 frames. **Record** stays grey until data arrives.*

1. Set **Start at frame**. **Current** uses the playhead.
2. Optionally tick **Stop at frame** (default `30`) to record into that range only ("punch in"). Keys
   outside the range are left alone.
3. Set **Countdown** (0–5 seconds, default 3) to get into position.
4. Optionally tick **Selected body parts only** to record only the parts of the selected bones, for
   example new arms over an existing walk, or **Face only** to record only the face bones (and the head
   from an iPhone). The two are exclusive.
5. Press **Record**. During the countdown, **Cancel** stops it. After the countdown the window shows
   **Recording frame N**; press **Stop** to end the take.

Each take is one undo step. The clip's length grows to fit a take that runs past the last frame. A take
is cancelled if you open or create another document, or switch to another actor (see
[[Couples and groups]]), before it ends.

### Hip movement

A take moves the hips (`mPelvis`) by how far the performer moves away from standing, scaled by the
avatar's hip-to-ankle height over the performer's. Second Life plays that movement from where the avatar
stands, so standing still plays at the avatar's own height.

- Without a captured rest pose, a take starts where you stand when it starts: the hips start with no
  forward or sideways movement, and a step forward moves them forward.
- Height is measured from you standing upright on the floor under your feet. A take that starts in a
  crouch starts with the hips low; the rest of the take is not raised.
- After **Capture Rest Pose Now**, takes measure the hips from where you stood when you pressed it. A take
  that starts a step in front of that spot starts a step forward.
- The live view measures from the captured rest pose, or from where you stood when data first arrived.

### Cleaning up a take

The **Clean-up** settings apply to the next take:

| Setting | Range | Default | Effect |
|---|---|---|---|
| **Smoothing** | **Off**, **Box (average)**, **One-Euro**, **Savitzky-Golay**, **Butterworth** | **Off** | calms tracker jitter; see below |
| **Reduce keys** | degrees, millimetres | on, `0.5` deg, `2.0` mm | removes keys that don't change the motion by more than these amounts |
| **Edge blend** | 0–15 frames | 4 | eases a punched-in take in and out of the animation around it |
| **Clean up foot sliding** | on/off | off | holds planted feet still with leg [[IK]] where the take had them on the ground; see [[Retargeting#Clean up foot sliding]] |
| **Heel and toe** | on/off | on | holds the heel and the toe separately, so a heel-toe roll hands over from heel to toe; off, the ankle alone |

With **Clean up foot sliding** on, the **Last take** report gives the take's ground height (for example
`ground: 1.2 cm above the floor`) and how far the hips were lowered where a leg could not reach its held
foot. A take has no **Put feet on the ground** tick, because moving the hips would move the whole
animation around a punched-in take too. Use **Tools → Clean Up Foot Sliding...** on the clip afterwards.

**Smoothing** choices:

- **Box (average)** averages each rotation with its neighbours over a **Box radius** of 1–5 frames. It is
  the filter older versions had; settings saved by them open with it.
- **One-Euro**, **Savitzky-Golay** and **Butterworth** filter the take's curves after it is fitted to the
  skeleton, before key reduction: every rotation curve (degrees) and position curve (metres, such as the
  hips' travel), face bones included. Their settings are the same as in the graph editor's **Filter
  Curves...**; see [[Graph editor#Filtering curves]] for what each one does. The ends of the take are
  padded by reflection.

With one of the three filters, **Last take** also gives the take's shake before and after the filter: the
mean over its bones, the three shakiest bones, and the hips' travel in m/s³ when it has any. Shake is the
RMS of the jerk (the third difference of each curve), in degrees per second cubed.

**Last take** reports what the last take recorded.

A take has a key on every frame. To edit it by hand, or to see what each clean-up step changes:

1. **Filter Curves...** in the [[Graph editor#Filtering curves|graph editor]] calms jitter that the take's
   **Smoothing** did not remove.
2. **Tools → Clean Up Foot Sliding** plants the feet, if the take's own clean-up did not.
3. **Edit → Simplify Curves...** turns the dense keys back into a few, within a tolerance; see
   [[Graph editor#Simplifying curves]]. Tick **Keep frames where feet are planted** to keep a key where
   each foot plants and lifts.

**Tools → Motion Quality...** shows the shake, the foot slide, the keys and the size before and after each
of these steps; see [[Motion quality]].

## Tips and tricks

- Record the body and the face in separate takes: record the body first, then a **Face only** take over
  it.
- Use **Stop at frame** with **Edge blend** to replace a few seconds in the middle of a good take.
- VATs redraws continuously only while it is listening. Click **Stop Listening** when you are done, and
  it goes back to using no CPU when idle.

> **Note:** The window's settings are saved in `settings.json` as you change them: source, port, **Allow
> Other Devices**, phone address, **Drive the avatar**, the face settings, **Countdown** and the
> **Clean-up** section. The start and stop frames, **Selected body parts only** and **Face only** are
> chosen per take and not saved.

## Troubleshooting

### No data arrives

The sender uses another address or port, or a firewall drops the packets. Check that **Your computer**
matches the address in the sending app and that both use the same port. For a phone or another computer,
tick **Allow other devices** and follow the firewall line of the checklist. The phone and the computer
must be on the same network.

### Arms or legs come in twisted

The sender's rest pose differs from VATs'. Stand in a T-pose and press **Capture Rest Pose Now**.

### The take starts away from the avatar

A rest pose was captured at another spot, and takes measure the hips from there. Stand where you will
record and press **Capture Rest Pose Now** again. With the VMC protocol, **Reset to T-Pose** instead makes
each take start where you stand.

### The take was cancelled

The document or the edited actor changed while recording. Record again without switching.

## See also

- [[Face tracking]]
- [[Motion quality]]
- [[Retargeting]] for recorded files (BVH, glTF, FBX) instead of live streams
- [[VATs Editor (viewer)]]
- [VMC protocol specification](https://protocol.vmc.info/english)

Category: Motion
