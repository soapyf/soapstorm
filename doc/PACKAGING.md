# SoapStorm Packaging & Release Guide

This document details the architecture, build procedure, and release workflow for packaging SoapStorm updates with **Velopack** differential updates.

---

## 1. Overview & Architecture

SoapStorm uses **Velopack** on Windows and macOS to distribute fast, differential (delta) updates.

* **Install Location**: `%LocalAppData%\Soapstorm` (or `Soapstorm-Beta`).
* **Why AppData?**: Installing to user space avoids Windows UAC / Administrator prompts on updates. The viewer can silently download delta patches (`.nupkg`), reconstruct the updated package in the background, and seamlessly apply the update on restart.
* **Preservation of User Settings**: All user settings, keybindings, graphics options, chat logs, and credentials reside in `%AppData%\Soapstorm_x64` and are unaffected by version upgrades.
* **Differential Deltas**: Velopack creates `.delta.nupkg` files (typically 10–20 MB) that only package binary diffs (via `zsdiff`) of changed executables and assets against previous versions, rather than redownloading the entire ~380 MB bundle.

---

## 2. Release Channels

* **Beta / Pre-release**: Published to GitHub Releases with the **"Set as a pre-release"** flag checked.
* **Stable / General Release**: Published to GitHub Releases as a standard release (pre-release unchecked).
* **In-Viewer Selection**:
  * In **Preferences → Soapstorm → General (and Login)**, users have a checkbox: **"Include beta / pre-release updates"** (`UpdaterWillingToTest`).
  * If checked, the viewer queries the top 10 GitHub releases and picks the highest semantic version (including pre-releases).
  * If unchecked, the viewer queries only official releases (`/releases/latest`).
* **Promotion**: You can promote a Beta release to Stable at any time by editing the release on GitHub and unchecking **"Set as a pre-release"** without having to recompile.

---

## 3. Step-by-Step Packaging Procedure (Windows)

### Step 1: Version Bumping
1. Update `LL_VIEWER_VERSION_BUILD` in `indra/newview/llversioninfovars.h` (or your build directory's `build-vc171-64/newview/llversioninfovars.h`):
   ```c++
   #define LL_VIEWER_VERSION_BUILD 83391
   ```
2. Update the resource file `indra/newview/viewerRes.rc` (and/or `build-vc171-64/newview/viewerRes.rc`):
   * Bump `FILEVERSION`, `PRODUCTVERSION`, `FileVersion`, and `ProductVersion` to match `7,3,0,83391`.
3. If doing an incremental build, recompile the resource file:
   ```powershell
   & "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\rc.exe" `
     /I "indra\newview\res" `
     /I "C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um" `
     /I "C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\shared" `
     /fo "build-vc171-64\newview\firestorm-bin.dir\Release\viewerRes.res" `
     "build-vc171-64\newview\viewerRes.rc"
   ```

### Step 2: Build the Viewer Executable
Compile the `firestorm-bin` project in Release configuration:
```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe" `
  build-vc171-64\newview\firestorm-bin.vcxproj `
  /p:Configuration=Release `
  /p:Platform=x64 `
  -m
```
Copy the newly built binary to the main packaging executable:
```powershell
Copy-Item "build-vc171-64\newview\Release\firestorm-bin.exe" "build-vc171-64\newview\Release\Soapstorm-Beta.exe" -Force
```

### Step 3: Package with Velopack CLI (`vpk`)
> [!IMPORTANT]
> Keep previous `.nupkg` releases inside the output directory (`build-vc171-64\newview\Releases`). Velopack inspects prior packages in this directory to generate the differential `.delta.nupkg`.

Execute `vpk pack`:
```powershell
& "$env:USERPROFILE\.dotnet\tools\vpk.exe" pack `
  --packId Soapstorm-Beta `
  --packVersion 7.3.0-83391 `
  --packDir C:\Users\dsull\source\repos\soapstorm\build-vc171-64\newview\Release `
  --mainExe Soapstorm-Beta.exe `
  --packTitle Soapstorm-Beta `
  --exclude ".*\.pdb|.*\.map|.*\.bat|.*\.exp|.*\.lib|.*\.nsi|.*\.tar\.xz|firestorm-bin\..*|.*_Setup\.exe|.*-Setup\.exe" `
  -xy `
  --shortcuts None `
  --runtime win-x64 `
  --icon indra\newview\icons\beta\soapstorm_icon.ico `
  --outputDir C:\Users\dsull\source\repos\soapstorm\build-vc171-64\newview\Releases
```

This generates:
* `Soapstorm-Beta-7.3.0-83391-delta.nupkg` (~10–20 MB)
* `Soapstorm-Beta-7.3.0-83391-full.nupkg` (~380 MB)
* `Soapstorm-Beta-win-Setup.exe` (Standalone full installer)
* Updated `releases.win.json` and `RELEASES`

---

## 4. Publishing to GitHub Releases

Every release published to GitHub must include all 5 core assets for the update pipeline to function:

```powershell
gh release create v7.3.0.83391 `
  --title "SoapStorm Beta 7.3.0.83391" `
  --notes "Release notes describing the changes..." `
  --prerelease `
  build-vc171-64\newview\Releases\Soapstorm-Beta-7.3.0-83391-delta.nupkg `
  build-vc171-64\newview\Releases\Soapstorm-Beta-7.3.0-83391-full.nupkg `
  build-vc171-64\newview\Releases\releases.win.json `
  build-vc171-64\newview\Releases\RELEASES `
  build-vc171-64\newview\Releases\Soapstorm-Beta-win-Setup.exe
```

> [!TIP]
> If creating an official stable release instead of a beta, omit the `--prerelease` flag.

---

## 5. Testing Local Differential Updates

To test an update before releasing to all users:
1. Ensure the installed executable in `%LocalAppData%\Soapstorm-Beta\current\Soapstorm-Beta.exe` and `%LocalAppData%\Soapstorm-Beta\current\sq.version` are on version `N` (e.g. `83390`).
2. Do **not** overwrite the installed executable with the new version `N+1` beforehand.
3. Publish version `N+1` (`83391`) to GitHub Releases.
4. Launch `Soapstorm-Beta` (version `N`), open **Preferences → Soapstorm → General**, and click **Check for Updates Now**.
5. Click **Download Now** and observe the live milestone transitions:
   * **0%–45%**: Patch download progress
   * **50%**: *"Extracting base package..."*
   * **65%**: *"Preparing differential patch..."*
   * **75%**: *"Applying differential patch..."*
   * **85%**: *"Assembling updated package..."*
   * **98%**: *"Verifying package..."*
   * **100%**: Prompt to *"Install Now"*.
