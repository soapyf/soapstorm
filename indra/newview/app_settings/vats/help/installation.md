# Installation

VATs is distributed as a folder you unpack and run: a `.tar.xz` archive for Linux and a `.zip`
archive for Windows. Nothing needs administrator rights, and nothing is written outside your home
folder unless you ask for it.

> Related articles: [[First steps]], [[Projects and files]], [[Troubleshooting]]

## Requirements

- A graphics driver with **OpenGL 3.3** (core profile) or newer.
- Linux: x86_64, glibc 2.35 or newer (Ubuntu 22.04 and later), X11 or Wayland. SDL3 is included in
  the archive as `bin/libSDL3.so.0`.
- Windows: 64-bit Windows.

A macOS build is not yet available.

## Usage

### Linux

1. Download `viewport-avatar-toolset-<version>-linux-x86_64.tar.xz`.
2. Unpack it anywhere, for example in `~/Applications`:
   `tar -xf viewport-avatar-toolset-<version>-linux-x86_64.tar.xz -C ~/Applications`
3. Run `bin/vats` from the unpacked folder.

The unpacked folder holds `bin/vats`, `share/viewport-avatar-toolset/` (the skeleton data, fonts,
starter props and desktop files), `share/icons/` and `INSTALL.txt`. Keep them together: VATs looks
for its data beside the executable.

### Windows

1. Download `viewport-avatar-toolset-<version>-windows-x64.zip`.
2. Unpack it anywhere.
3. Run `bin\vats.exe`.

### Updating

Unpack the new version over the old folder, or next to it. Your settings, libraries and autosaves
live in your home folder, not in the program folder, so every version finds them; see
[[Projects and files#Data folders]].

### Uninstalling

1. If you registered the file types, press **Remove** in **Edit → Preferences... → Project files**
   first.
2. Delete the program folder.
3. To remove your data too, delete the folders listed in [[Projects and files#Data folders]].

## Configuration

### Opening project files by double-click

**Edit → Preferences... → Project files → Open .vat Files with VATs** registers the file types
for the current user, pointing at the copy of VATs you are running. **Remove** undoes it.

| | Linux | Windows |
|---|---|---|
| Registers | `.vat` | `.vat` |
| Writes | `~/.local/share/applications/viewport-avatar-toolset.desktop`, `~/.local/share/mime/packages/viewport-avatar-toolset.xml` and the VATs icon in every size under `~/.local/share/icons/hicolor/`, then runs `update-mime-database`, `update-desktop-database` and `xdg-mime default` | keys under `HKEY_CURRENT_USER\Software\Classes`; the icon comes from `vats.exe` |
| Remove | deletes those files, icons included | deletes VATs' keys and the extensions' link to them; an extension that another program has taken since is left alone |

If you move the program folder, press the button again so the entry points at the new place.

### Menu entry and icons (Linux)

The in-app button installs the menu entry and the icon (the file types use the same icon). To do the
same by hand, follow `INSTALL.txt` in the unpacked folder. From that folder, it runs:

```
mkdir -p ~/.local/share/applications ~/.local/share/mime/packages
sed "s|^Exec=.*|Exec=$PWD/bin/vats %F|" share/viewport-avatar-toolset/packaging/viewport-avatar-toolset.desktop \
    > ~/.local/share/applications/viewport-avatar-toolset.desktop
cp share/viewport-avatar-toolset/packaging/viewport-avatar-toolset.xml ~/.local/share/mime/packages/
mkdir -p ~/.local/share/icons && cp -r share/icons/hicolor ~/.local/share/icons/
update-mime-database ~/.local/share/mime; update-desktop-database ~/.local/share/applications
```

## Troubleshooting

### "OpenGL 3.3 is not available"

The graphics driver offers an older OpenGL, or none. Update the driver. Virtual machines and some
remote-desktop sessions offer only an old OpenGL; see [[Troubleshooting#VATs does not start]].

### Double-clicking a .vat file opens something else

Another program owns the file type, or the VATs entry points at a folder that has moved. Press
**Open .vat Files with VATs** again in the copy you want to use.

## See also

- [[First steps]]
- [[Command line]]
- [[Troubleshooting]]

Category: Getting started
