# FAQ

Short answers to common questions about VATs, with links to the pages that explain more.

> Related articles: [[VATs]], [[Troubleshooting]], [[First steps]]

## General

### Is VATs free?

Yes. VATs is open source under the GNU Lesser General Public License 2.1. The Second Life skeleton,
attachment points and avatar meshes come from Linden Lab's viewer under the same licence. The starter
props are third-party models under CC0 1.0 or CC BY 3.0; the credits file ships with them, and
**Help → About Viewport Avatar Toolset** lists the rest.

### Can I sell animations I make with VATs?

The licence covers VATs' own code and data, not the animations you make with it. What you animate
is yours. Props and mesh bodies you import keep their own licences.

### Which systems does it run on?

Linux (x86_64, glibc 2.35 or newer) and 64-bit Windows, with OpenGL 3.3. A macOS build is not yet
available. See [[Installation]].

### Does VATs need the internet?

No. VATs makes no internet connections. [[Motion capture]] listens for data from capture apps on your
own computer or local network.

### What is the difference between VATs and the VATs Animator?

Viewport Avatar Toolset is the standalone app. The VATs Animator is the same core inside the SoapStorm
viewer, animating your own avatar in-world. They read and write the same `.vat` projects. See
[[VATs]] and [[VATs Animator (viewer)]].

## Coming from other programs

### Can I open my Hexton projects?

Yes. **File → Open...** reads `.hxanim` projects and converts them; save the result as a new `.vat`
file. On the first start, VATs also offers to import Hexton's preferences, recent files and
libraries. See [[Hexton import]].

### Can I keep my Maya, Blender or QAvimator habits?

Yes. Pick a preset in **Edit → Preferences... → Navigation & hotkeys**. See [[Control presets]].

### Can I change individual keys?

No. Keys come from the four presets, which cannot be edited one key at a time. See
[[Keyboard shortcuts]].

### Can I use a pen tablet or a mouse without a middle button?

Yes. The QAvimator and Second Life presets orbit, pan and zoom with the left button and modifier
keys. The Industry preset needs a middle button to pan. With the Blender preset, tick **Emulate
3-button mouse (Alt + left-drag = middle-drag)** in [[Preferences]]. See [[Control presets]].

### Can I bring in motion from Mixamo, Rokoko or other BVH and FBX files?

Yes. **File → Import BVH...** reads BVH files; **File → Import Animation (Retarget)...**
maps motion from other skeletons (`.bvh`, `.fbx`, `.gltf`, `.glb`). See [[BVH]] and [[Retargeting]].

## Animating

### Does VATs support Bento bones?

Yes: hands, face, wings, tail, hind limbs and groin bones, and the attachment points. **View → Show
... Bones** switches each group. See [[Skeleton]].

### Why does my pose look different on my own avatar?

Your shape and mesh body differ from the Linden default. Preview on your mesh body with **View →
Body**, and choose the bake shape in the export settings. See [[Mesh bodies]] and
[[Export to Second Life]].

### Can I animate two avatars together?

Yes, with actors: **Tools → Actors (Couples and Groups)...**. See [[Couples and groups]].

### Does undo cover everything?

**Ctrl+Z** undoes edits to the animation and its props. Camera moves and selection are not undone.

## Files and export

### Should I export .anim or BVH?

`.anim`. It is Second Life's own format and keeps per-bone priority, attachment points, the hand
pose and the ease settings. BVH is for other tools. See [[Anim format]] and [[BVH]].

### Where is my project saved while I work?

Only where you save it. VATs also autosaves unsaved work to its data folder (a minute after the first
change, then every two minutes), and offers it back after a crash. See [[Projects and files#Autosave]].

### Where are my settings?

`~/.config/viewport-avatar-toolset/settings.json` on Linux and
`%APPDATA%\viewport-avatar-toolset\settings.json` on Windows. See [[Preferences#Settings file]].

## See also

- [[Troubleshooting]]
- [[VATs]]

Category: Reference
