**[Firestorm](https://www.firestormviewer.org) is a free client for 3D virtual worlds such as Second Life and various OpenSim worlds where users can create, connect and chat with others from around the world.**

## About This Fork

**This is a modified fork of the Firestorm viewer that I am using to mess with ideas and learning. If you've arrived here in search of the offical firestorm viewer you can find it at the [official website](https://www.firestormviewer.org/choose-your-platform/)**

### What's Changed

**Experience Management Improvements:**
- Users can now click "Forget" to leave privileged experiences
- Users can block privileged experiences if desired
- The "Privileged" label still displays for transparency

**Mouselook Zoom Improvements:**
- Added smooth FOV transitions for right-click zoom in mouselook
- New preference slider: "Mouselook zoom transition" (0-5000ms) under Move & View > Mouselook tab
- New preference slider: "Mouselook zoomed sensitivity" (0-100) under Move & View > Mouselook tab
- Proportional zoom-out timing: Partial zooms return faster (e.g., 50% zoom = 2x faster zoom-out)
- Fixed bug where camera stays zoomed after death or teleport
- Preserves user's mousewheel-adjusted zoom preferences across sessions

**Texture Synchronization Improvements:**
- Added "Sync" button to texture panels for explicit per-face material synchronization
- Button copies diffuse texture offset, repeats, and rotation to normal and specular for all selected faces

**Keybinding Changes:**
- Changed Ctrl+U keyboard shortcut from single image upload to bulk uploader

---

This repository is based on the official Firestorm viewer source code.

