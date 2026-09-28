# Command line

VATs takes options and file names on the command line. Most options exist for scripted screenshots,
tests and benchmarks; `--data-dir` and file names are also useful day to day.

> Related articles: [[Projects and files]], [[Preferences]], [[Troubleshooting]]

## Usage

```
vats [options] [files]
```

On Windows the program is `bin\vats.exe`. Options and files are handled from left to right, after
the window and the skeleton have loaded; `--data-dir` and `--library-dir` are read first, before
anything loads.

### Files

Any argument that names an existing file is opened as if it were dropped on the window: `.vat` and
`.hxanim` open as projects, `.anim` and `.bvh` are imported, `.dae` and `.fbx` are added as props,
`.gltf` and `.glb` open the retarget dialog, and `.wav`, `.mp3`, `.ogg` and `.flac` load as the audio
track. An
argument that is neither an option nor an existing file is skipped with
`ignoring unknown argument <arg>` on standard error.

Because arguments run in order, put the project before options that act on it:

```
vats walk.vat --frame 12 --select mPelvis
```

### Options

| Option | Effect |
|---|---|
| `--data-dir <dir>` | Keeps all user data in `<dir>`: `settings.json`, `layout.ini`, `autosave/` and `library/`. The folder is created if needed. See [[Projects and files#Data folders]]. |
| `--library-dir <dir>` | Uses `<dir>` for the pose, prop and mesh body libraries and their thumbnails instead of `library/` in the data folder. |
| `--preset <name>` | Selects a [[Control presets|control preset]]: `industry`, `blender`, `qavimator` or `secondlife`. An unknown name prints `unknown preset <name>`. |
| `--body <id>` | Shows a body for this run only: `sl-default`, `sl-default-male`, `female`, `male`, or `none` (also `off`) for the skeleton only. It replaces a mesh body too. The body saved in `settings.json` is kept unless you pick another in **View → Body**. Unknown ids are ignored. |
| `--frame <n>` | Moves to frame `<n>`, clamped to the animation's length. Fractions are allowed. |
| `--select <bone>` | Selects a bone by its skeleton name, for example `mPelvis` or `mHandLeft`. Unknown names are ignored. |
| `--select-all` | Runs **Select All**. |
| `--select-prop <n>` | Selects the project's prop number `<n>`, counting from 0. Out of range is ignored. |
| `--pose <slug>` | Applies a starter pose at the current frame, for example `body-sit` or `hand-fist`. An unknown slug prints `no built-in pose <slug>`. |
| `--tool <name>` | Picks a tool: `select`, `move`, `rotate` or `scale`. Any other value picks `rotate`. |
| `--focus` | Frames the selection once the first frame is drawn, as **Frame Selected**. |
| `--distance <m>` | Sets the camera's distance from its target, in metres, on the first frame. |
| `--points` | Shows the attachment points, as **View → Show Attachment Points**. |
| `--tab <name>` | Brings a left panel to the front: `bones` for **Bones**; `actors` opens the **Actors** window instead; any other value for **Inventory**. |
| `--import-prop <file>` | Imports a `.dae` or `.fbx` file as a prop, as **File → Import Prop / Mesh (.dae, .fbx)...**. |
| `--open-help <page>` | Opens the help at a page, by title or file name; `<page>#<heading>` opens it at a heading. |
| `--screenshot <file.png>` | Runs without dialogs, draws 12 frames, saves the window as a PNG and quits. See [[Command line#Screenshots]]. |
| `--bench <seconds>` | Plays the animation with vsync off, times each part of the frame for `<seconds>`, prints the results and quits. See [[Command line#Benchmarks]]. |

Starter pose slugs: body poses `body-stand`, `body-hips`, `body-arms-crossed`, `body-thinking`,
`body-wave`, `body-sit`, `body-contrapposto`; hand poses include `hand-relaxed`, `hand-rest`,
`hand-open`, `hand-flat`, `hand-fist`, `hand-loose-fist`, `hand-point`, `hand-peace`, `hand-thumbs-up`,
`hand-ok`, `hand-pinch`, `hand-grip` and `hand-pen`. A hand pose is applied to both hands. See
[[Pose library]].

### Screenshots

With `--screenshot`, VATs runs headless: no message boxes, no Welcome window, no Hexton import offer,
no **Recover unsaved work** window, no autosave, no "Save changes?" prompt, and opened files are not
added to the recent list. Old autosaves stay where they are for the next normal start. Error messages go
to standard error. The PNG has the window's size in pixels.

```
vats --data-dir /tmp/vats-shot walk.vat --frame 20 --select mHandRight --focus --screenshot hand.png
```

The window still opens, so a display is needed. On Linux without a desktop, run it under a virtual X
server such as Xvfb:

```
env -u WAYLAND_DISPLAY DISPLAY=:93 vats --screenshot out.png
```

> **Tip:** Use `--data-dir` with a scratch folder for scripted runs, so the run starts from default
> settings and layout rather than your own.

### Benchmarks

`--bench <seconds>` starts playback with vsync off, skips 60 warm-up frames, then times the frame's
sections for the given number of seconds. It prints one line per section with the median, 95th
percentile and mean in milliseconds, and the sample count, then quits.

```
vats walk.vat --bench 10
```

## Troubleshooting

### Options seem to be ignored

- A misspelt option is taken as a file name and skipped: look for `ignoring unknown argument` on
  standard error. On Windows, VATs has no console window; redirect standard error to a file to see it (see
  [[Troubleshooting#Logs]]).
- An option before the project file acts on the empty document, and the file then replaces it. Put
  the file first.

### A --preset choice stays after the run

`--preset` changes the preset in memory only, but any setting saved later in the same run (a theme,
a stored camera view) writes the whole settings file, including that preset. Use `--data-dir`
for trial runs.

## See also

- [[Projects and files]]
- [[Control presets]]
- [[Troubleshooting]]

Category: Reference
