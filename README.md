<img align="left" width="100" height="100" src="doc/soapstorm_256.png" alt="Logo of Soapstorm viewer"/>

# Soapstorm Viewer

**[Soapstorm](https://github.com/soapyf/soapstorm) is an experimental fork of the [Firestorm Viewer](https://www.firestormviewer.org), a client for 3D virtual worlds such as Second Life and OpenSim.**

---

> [!IMPORTANT]
> **Looking for official Firestorm?**
> This repository is a personal fork used for learning, exploring new ideas, and prototyping custom features. It is not an official release of Firestorm. If you are looking for the official, supported Firestorm viewer, please download it from the [Official Firestorm Website](https://www.firestormviewer.org/choose-your-platform/).

---

## About This Fork

Soapstorm builds upon Firestorm to experiment with enhanced photography and avatar tools, combat and camera controls, UI quality-of-life additions, and stability improvements:

- **Photography & Avatar Snapshot Suite:**
  - Transparent avatar snapshot capture with holdout alpha mattes (clean avatar cutouts).
  - Blender-style orthographic camera toggle with automatic perspective revert upon camera motion.
  - Dedicated portrait aspect ratio swap and 90° roll controls directly on camera floaters.
  - Selective avatar capture scopes (*Self*, *Self + Selected*, *All Avatars*).
  - Categorized widescreen (16:9) and legacy (4:3) snapshot resolution presets up to 8K.
- **Over-the-Shoulder (OTS) Camera & Combat Controls:**
  - Over-the-shoulder (OTS) camera mode with collision detection, jitter smoothing, and shoulder-swap keybind.
  - Fair-fire aim convergence (aim converges on the avatar under the crosshair rather than background geometry) and true-aim crosshair.
  - Combat feedback overlays including hitmarkers, hit reports, custom crosshairs, and LSL-fed kill feed.
- **Controls & Interaction:**
  - Smooth mouselook zoom transitions with configurable duration and sensitivity sliders.
  - Right mouse button hold tracking in mouselook for script events.
  - Avatar Poser feature allowing you to pose avatars who have granted modification rights.
- **Workflow & UI Improvements:**
  - Material texture synchronization across selected faces.
  - Quick bulk upload shortcut (Ctrl+U).
  - Group-based nameplate tinting and dedicated Group Viewer floater.
  - Customized login splash and built-in auto-update notification checks.
  - Native Linux dialog support (`zenity` / `kdialog`) for Wayland and KDE Plasma environments.
- **Stability & Performance:**
  - Shutdown path error-handling improvements to prevent settings loss on abort.
  - Fixes for texture purging and background file I/O safety.

---

## Build Instructions

For building from source, refer to the platform guides in [`doc/`](doc/):

- [Windows](doc/building_windows.md)
- [macOS](doc/building_macos.md)
- [Linux](doc/building_linux.md)

> [!NOTE]
> This is a personal development fork. For official compiler support and community discussions regarding compiling Firestorm, visit the [Firestorm Self Compilers Group](https://tinyurl.com/firestorm-self-compilers).

---

## Upstream & Licensing

- **Upstream:** Derived from the open-source [Firestorm Viewer](https://github.com/FirestormViewer/phoenix-firestorm), which in turn derives from Linden Lab's [Second Life Viewer](https://github.com/secondlife/viewer).
- **License:** Licensed under the [GNU Lesser General Public License (LGPL)](doc/LGPL-license.txt).
- **Policies:** Guided by Linden Lab's [Third-Party Viewer (TPV) Policy](https://secondlife.com/corporate/third-party-viewers) and [Code of Conduct](https://github.com/secondlife/viewer?tab=coc-ov-file).
