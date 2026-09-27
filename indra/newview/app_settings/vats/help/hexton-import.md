# Hexton import

VATs opens project files from Hexton SL Animator (`.hxanim`) and can bring over that app's preferences,
recent files and libraries. This page describes what is converted and where VATs looks for the data.
VATs is a separate program and shares no code with Hexton SL Animator.

> Related articles: [[Projects and files]], [[Project file format]], [[Pose library]], [[Props]]

## Usage

### Open an .hxanim project

Choose **File → Open...** and pick the file; the file type list has **VATs project** and **Hexton
project**. Double-clicking the file in the file manager also works once file types are registered (see
[[Hexton import#File association]]).

VATs converts the file into a new, untitled VATs project. The status bar says `Opened <file> (converted
from Hexton; Save As to keep it)`. The `.hxanim` file is only read: saving asks for a new name and
writes a `.vat` file, never over the original.

What the conversion keeps:

- every field of the file, mapped onto the matching VATs setting: keys, IK, pins, props and export
  settings;
- IK as the original app solved it, so limbs pose the same as before. The project keeps this after it
  is saved as `.vat`;
- prop mesh paths, made relative to the new project where possible;
- the source file's path, and any field VATs does not know, stored in the project's metadata so
  nothing is lost.

### Import preferences and libraries

On the first start, VATs looks for Hexton SL Animator's data folder. If it finds one, it asks **Import
from Hexton SL Animator?** and shows the folder.

- **Import** brings over:
  - preferences from `settings.cfg`: control preset, interface size, gizmo size, rotation snap, view
    cube size, orientation, theme, body, and whether the graph and the start screen are shown;
  - recent files that still exist, up to 10;
  - the pose library (`library/poses.json`) and the prop library (`library/library.json`), only if
    VATs has no library of its own yet. Nothing is overwritten. Thumbnails are not copied; VATs
    renders its own.
- **Don't import** closes the prompt.

VATs asks once. A message lists what was imported, or says `Nothing new to import.`

### Data folder locations

| System | Folder |
|---|---|
| Linux | `~/.local/share/godot/app_userdata/Hexton SL Animator 2026/` (or under `$XDG_DATA_HOME/godot/`) |
| Windows | `%APPDATA%\Godot\app_userdata\Hexton SL Animator 2026\` |
| macOS | `~/Library/Application Support/Godot/app_userdata/Hexton SL Animator 2026/` |

### File association

**Edit → Preferences... → Project files → Open .vat Files with VATs** registers both `.vat` and
`.hxanim` with the desktop, so double-clicking either opens VATs. **Remove** undoes it.

## Troubleshooting

### VATs did not offer to import

The prompt appears only on the first start, and only when the data folder exists at the path above. If
you pressed **Don't import**, VATs does not ask again.

### The theme changed after the import

Hexton's **Maya Gray** theme maps to VATs' **Studio Grey**; every other theme maps to **Dusk**.

### Saving a converted project asks for a file name

This is intended. The `.hxanim` is kept unchanged; the converted project is a new `.vat` file.

Category: Import and export
