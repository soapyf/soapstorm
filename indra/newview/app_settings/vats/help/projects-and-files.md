# Projects and files

VATs saves your work as a `.vat` project: the animation with every key, the props, the audio track,
the actors and the export settings. It keeps a backup of the previous save, autosaves unsaved work,
and offers that work back after a crash. Your libraries and settings live in data folders in your
home folder, separate from projects.

> Related articles: [[Project file format]], [[Preferences]], [[Export to Second Life]], [[Troubleshooting]]

## Usage

### Saving

- **File → Save** (**Ctrl+S**) writes the project to its file. A project without a file asks for a
  name first.
- **File → Save As...** (**Ctrl+Shift+S**; **Ctrl+A** in QAvimator) saves under a new name. The dialog
  starts beside the current project, and VATs adds `.vat` when the name has no extension.

Each save writes a temporary file and then renames it over the project, so a failed save never leaves
half a file. The previous version is kept beside it as `<name>.vat.bak`, replacing the older
backup. If the save fails, the **Save failed** message gives the reason, and the file on disk is
unchanged.

Prop meshes and the audio file are stored relative to the project where possible, so a project folder
can be moved or copied with its files.

The title bar shows `*` after the file name while there are unsaved changes.

### Opening

- **File → Open...** (**Ctrl+O**) opens a VATs project (`.vat`) or a Hexton project (`.hxanim`).
- **File → Open Recent** lists up to 10 recent projects, newest first, numbered. Files that no longer
  exist are hidden. **Clear Recent** empties the list.
- Drop a file on the VATs window: a project opens, a `.anim` or `.bvh` file is imported, a `.dae`
  or `.fbx` file is added as a prop, a `.gltf` or `.glb` file opens the [[Retargeting|retarget]] dialog,
  and a `.wav`, `.mp3`, `.ogg` or `.flac` file is loaded as the [[Audio track]].
- Name files on the [[Command line]]: they are opened the same way.

Only one project is open at a time. Opening, importing an animation or **File → New** with unsaved
changes asks `Save changes to <name>?` with **Save**, **Don't Save** and **Cancel**. **Save** on a
project that has never been saved opens **Save As...**; once it has saved, the command you started
goes on. Cancelling **Save As...** cancels the command too.

When you close the window or press **Ctrl+Q**, VATs asks the same question.

### Special cases when opening

| Case | What VATs does |
|---|---|
| A Hexton `.hxanim` project | converts it; the status bar says `(converted from Hexton; Save As to keep it)`. The project has no file name, so **Save** asks for one and the original is never overwritten. |
| A project saved by a newer VATs | opens what it can and shows **Newer project file**. **Save** asks for a new name, so the original stays whole. |
| Prop meshes missing | opens anyway and lists them in **Some prop meshes are missing**. The props show as orange boxes until the files are back. |
| An unreadable file | shows **Could not open project** with the reason; the open project is unchanged. |

### Autosave

While there are unsaved changes, VATs writes a copy of the project to the data folder one minute
after the first change, and again every two minutes while changes remain. Autosave does not touch
your project file.

The autosave is removed when you save, open or start another project, or quit normally (including
**Don't Save**). If the system stops VATs (logout, shutdown, a `SIGTERM`), VATs writes the autosave
at once and keeps it.

### Recovering after a crash

When VATs starts and finds an autosave from a session that did not end cleanly, the **Recover unsaved
work** window lists it with the project name (or `Untitled`) and its age:

- **Recover** opens it as unsaved work, with its original file name. The status bar says
  `Recovered <name>: save it to keep it`.
- **Discard** deletes that autosave.
- **Later** (or **Esc**) keeps them all and asks again at the next start.

Only one autosave is recovered at a time; the others are offered at the next start. The recovered
work is autosaved again before the old copy is deleted, so a second crash still leaves it.

> **Note:** An autosave is offered only once it is five minutes old, since a younger one may belong to
> a copy of VATs that is still running. After a crash, if the window does not appear, start VATs
> again a few minutes later.

### Importing from Hexton SL Animator

On the first start (when no `settings.json` exists yet), VATs looks for Hexton SL Animator's data
folder:

| System | Folder |
|---|---|
| Linux | `$XDG_DATA_HOME/godot/app_userdata/Hexton SL Animator 2026/`, normally under `~/.local/share` |
| Windows | `%APPDATA%\Godot\app_userdata\Hexton SL Animator 2026\` |

If it is there, the **Import from Hexton SL Animator?** window offers to bring across your pose library,
prop library, recent files and preferences:

- **Import** copies them and reports what was brought over. Preferences include the control preset,
  3-button emulation, interface size, gizmo size, snap angle, view cube size, graph visibility, the
  Welcome setting, gizmo axes, colour theme and body. A library is copied only when VATs has none
  yet, so nothing is overwritten. Library thumbnails are not copied; VATs draws its own.
- **Don't import** (or **Esc**) closes the window. VATs does not ask again.

Hexton projects can be opened at any time with **File → Open...**; see
[[Projects and files#Special cases when opening]] and [[Hexton import]].

## Data folders

| What | Linux | Windows |
|---|---|---|
| Settings (`settings.json`) | `~/.config/viewport-avatar-toolset/` | `%APPDATA%\viewport-avatar-toolset\` |
| Libraries, autosaves, window layout | `~/.local/share/viewport-avatar-toolset/` | `%APPDATA%\viewport-avatar-toolset\` |

On Linux the folders follow `$XDG_CONFIG_HOME` and `$XDG_DATA_HOME` when they are set. The data folder
holds:

| Path | Contents |
|---|---|
| `library/poses.json` | the pose library: poses and clips ([[Pose library]]) |
| `library/library.json` | the prop library ([[Props]]) |
| `library/bodies.json` | your mesh bodies ([[Mesh bodies]]) |
| `library/*.png` | Inventory thumbnails |
| `autosave/` | autosaves, a `.vat` and a `.path` file per session |
| `layout.ini` | the panel layout |

Copy the `library` folder to back up your libraries. If the pose or prop library cannot be read,
VATs renames it to `poses.json.corrupt-<number>` or `library.json.corrupt-<number>`, says so, and
starts an empty one, so the damaged file is never overwritten.

### Keeping everything in one folder

`--data-dir <dir>` puts settings, layout, autosaves and libraries all in `<dir>`, for example on a USB
stick. `--library-dir <dir>` moves only the libraries. See [[Command line]].

## Troubleshooting

### "Save failed"

The folder is read-only, the disk is full, or the file is open elsewhere (Windows). The message gives
the system's reason. Save to another folder with **Save As...**.

### My changes are gone after a crash

Start VATs again after five minutes and check the **Recover unsaved work** window. Changes made after the last autosave, up to
two minutes of work, are not in it. The `.vat.bak` file beside the project holds the save before the
last one: rename it to `.vat` to open it.

### Props show as orange boxes

VATs cannot find the prop mesh files. Put them back at the path shown in **Some prop meshes are
missing**, then open the project again.

## See also

- [[Project file format]]
- [[Preferences#Settings file]]
- [[Command line]]
- [[Troubleshooting]]

Category: Getting started
