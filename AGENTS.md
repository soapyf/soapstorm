# SoapStorm Agent Instructions & Guidelines

This file provides critical guidelines, architectural context, and procedural instructions for AI agents working in this repository.

---
 

## 1. Auto-Update Architecture (Velopack)

SoapStorm uses **Velopack** for differential auto-updates on Windows and macOS:
* **Installation Root**: `%LocalAppData%\Soapstorm` (or `Soapstorm-Beta`).
* **User Settings**: Stored in `%AppData%\Soapstorm_x64` (Windows) or `~/Library/Application Support/Soapstorm_x64` (macOS). Never modify user settings when installing, updating, or testing.
* **Update Engine**:
  * Core integration: [`indra/newview/llvelopack.cpp`](file:///c:/Users/dsull/source/repos/soapstorm/indra/newview/llvelopack.cpp) and [`indra/newview/llvelopack.h`](file:///c:/Users/dsull/source/repos/soapstorm/indra/newview/llvelopack.h).
  * Auto-update checker: [`indra/newview/llautoupdatechecker.cpp`](file:///c:/Users/dsull/source/repos/soapstorm/indra/newview/llautoupdatechecker.cpp).
  * Update UI & Progress: [`indra/newview/llfloaterupdateprogress.cpp`](file:///c:/Users/dsull/source/repos/soapstorm/indra/newview/llfloaterupdateprogress.cpp) and [`floater_update_progress.xml`](file:///c:/Users/dsull/source/repos/soapstorm/indra/newview/skins/default/xui/en/floater_update_progress.xml).
* **Differential Deltas**: Velopack generates `.delta.nupkg` packages (~10–20 MB) comparing the target version against previous `.full.nupkg` packages.
* **Milestone Progress Tracking**:
  * Velopack child process (`Update.exe`) performs patching out-of-process.
  * In `llvelopack.cpp`, a background watcher tails `%LocalAppData%\velopack\velopack_Soapstorm-Beta.log` using `LLFile::stat` and exact byte-range slicing to track child process milestones:
    * 0%–45%: Delta download
    * 50%: Base package extraction
    * 65%: Differential patch preparation
    * 75%: Applying differential patch (`zsdiff`)
    * 85%: Assembling output package
    * 98%: Package verification
    * 100%: Presentation of "Install Now" modal

---

## 2. Release & Packaging Procedure

Refer to [`doc/PACKAGING.md`](file:///c:/Users/dsull/source/repos/soapstorm/doc/PACKAGING.md) for full documentation. When packaging a new release:

1. **Version Bumping**:
   * Bump build number in `indra/newview/llversioninfovars.h` (and/or `build-vc171-64/newview/llversioninfovars.h`):
     `#define LL_VIEWER_VERSION_BUILD <commit_or_build_number>`
   * Bump `FILEVERSION`, `PRODUCTVERSION`, `FileVersion`, `ProductVersion` in `indra/newview/viewerRes.rc` (and/or `build-vc171-64/newview/viewerRes.rc`).
   * Recompile resource file with `rc.exe` before linking if performing an incremental build.

2. **Compiling**:
   * Compile project:
     `MSBuild.exe build-vc171-64\newview\firestorm-bin.vcxproj /p:Configuration=Release /p:Platform=x64 -m`
   * Copy `build-vc171-64\newview\Release\firestorm-bin.exe` to `build-vc171-64\newview\Release\Soapstorm-Beta.exe`.

3. **Packaging with `vpk`**:
   * **Crucial**: Keep prior `.nupkg` packages in `build-vc171-64\newview\Releases` so `vpk pack` can build the differential delta!
   * Command:
     ```powershell
     & "$env:USERPROFILE\.dotnet\tools\vpk.exe" pack `
       --packId Soapstorm-Beta `
       --packVersion 7.3.0-<build> `
       --packDir C:\Users\dsull\source\repos\soapstorm\build-vc171-64\newview\Release `
       --mainExe Soapstorm-Beta.exe `
       --packTitle Soapstorm-Beta `
       --exclude ".*\.pdb|.*\.map|.*\.bat|.*\.exp|.*\.lib|.*\.nsi|.*\.tar\.xz|firestorm-bin\..*|.*_Setup\.exe|.*-Setup\.exe" `
       -xy --shortcuts None --runtime win-x64 `
       --icon indra\newview\icons\beta\soapstorm_icon.ico `
       --outputDir C:\Users\dsull\source\repos\soapstorm\build-vc171-64\newview\Releases
     ```

4. **GitHub Releases**:
   * All 5 assets MUST be attached to the release:
     1. `Soapstorm-Beta-7.3.0-<build>-delta.nupkg`
     2. `Soapstorm-Beta-7.3.0-<build>-full.nupkg`
     3. `releases.win.json`
     4. `RELEASES`
     5. `Soapstorm-Beta-win-Setup.exe`
   * Add `--prerelease` for Beta builds.
   * Betas can be promoted to full releases directly on GitHub by unchecking "Set as a pre-release".

---

## 3. Testing Gotchas & Guidelines

* **Do Not Pre-Update the Local Executable When Testing**:
  * If testing an update from version `N` to `N+1`, the installed executable in `%LocalAppData%\Soapstorm-Beta\current\Soapstorm-Beta.exe` MUST remain at version `N`.
  * If the installed executable is prematurely overwritten with version `N+1`, `LLAutoUpdateChecker` will see `Current == Remote` and report "You are already on the latest version".
* **Do Not Add Artificial Progress Creep**:
  * The user prefers genuine milestone transitions over synthetic progress bar creeping.
* **Forward Slashes in Links**:
  * Always use forward slashes for markdown file links (e.g. `[file.cpp](file:///path/to/file.cpp)`).
