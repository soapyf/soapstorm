# SoapStorm Installation & Update Guide

This guide explains how to install, update, and manage SoapStorm on Windows, macOS, and Linux, including details on our modern differential auto-update system (powered by Velopack).

---

## Windows

### 1. New Auto-Updating System (Velopack)
SoapStorm uses **Velopack** for fast, differential updates.

* **Install Location**: `%LocalAppData%\Soapstorm` (or `Soapstorm-Beta`).
* **Why AppData instead of Program Files?**:
  `C:\Program Files` is protected by Windows and requires Administrator privileges (a UAC prompt) every time a file is changed. By installing into the user's local application data directory, SoapStorm can download small delta patches (`.nupkg`), apply changed files silently in the background, and restart immediately into the new version without interrupting you with Windows Administrator prompts.
* **Shortcuts**: Pinned taskbar icons, desktop shortcuts, and start menu entries automatically link to the active version.

### 2. Upgrading from Older Versions (Legacy NSIS)
If you previously installed SoapStorm using the legacy installer that placed files in `C:\Program Files\Soapstorm-Release`:

* **Your Settings Are Safe**: All graphics settings, chat logs, keybindings, account credentials, and UI preferences are stored in:
  `%AppData%\Soapstorm_x64\`
  This directory is completely separate from the program files and is automatically loaded by the new version.
* **Removing the Old Version**:
  Because deleting files from `Program Files` requires Administrator rights, the new installer does not silently delete the old folder. You can safely uninstall the older version whenever you want to free up disk space:
  1. Open **Windows Settings** (`Win + I`) → **Apps** → **Installed Apps** (or Control Panel → Programs and Features).
  2. Locate the older **SoapStorm** entry (the one located in `Program Files`).
  3. Click **Uninstall**. Your settings and chat logs will **not** be deleted.

---

## macOS

### 1. Installation
1. Download the `.dmg` disk image from the [SoapStorm GitHub Releases](https://github.com/soapyf/soapstorm/releases).
2. Open the `.dmg` and drag the **SoapStorm** application into your `/Applications` folder.
3. Launch SoapStorm from `/Applications` or Spotlight.

### 2. How Updates Work on macOS
* SoapStorm uses Velopack for macOS to deliver fast binary updates.
* When an update is ready, Velopack applies binary diffs directly to the `.app` bundle inside `/Applications` using its built-in macOS update helper (`UpdateMac`).
* User preferences, logs, and cache remain safe inside `~/Library/Application Support/Soapstorm_x64/`.

---

## Linux

### 1. Installation
1. Download the `Phoenix-Soapstorm-*.tar.xz` archive from GitHub Releases.
2. Extract the archive to your preferred location (e.g., `~/SoapStorm` or `/opt`).
3. Run the launch script (`./soapstorm`).

### 2. How Updates Work on Linux
* Linux installations do not use Velopack due to the diversity of package managers and distribution conventions across Linux distributions.
* Instead, the built-in update checker continuously monitors GitHub Releases. When a new version or beta is released, SoapStorm displays an in-viewer notification allowing you to download the new tarball with a single click.
* User data, settings, and logs are preserved in `~/.soapstorm_x64/`.

---

## Release Channels (Release vs. Beta)

In **Preferences (`Ctrl + P`) → Soapstorm → General (or Login)**:
* **Include beta / pre-release updates (Checked)**: You will receive early preview builds and test releases before they are made public.
* **Include beta / pre-release updates (Unchecked)**: You will only receive official stable releases.
* **Check for Updates Now**: Instantly queries GitHub to see if a newer version or patch is available for your chosen channel.
