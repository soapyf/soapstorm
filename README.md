<img align="left" width="100" height="100" src="doc/firestorm_256.png" alt="Logo of Firestorm viewer"/>

**[Firestorm](https://www.firestormviewer.org) is a free client for 3D virtual worlds such as Second Life and various OpenSim worlds where users can create, connect and chat with others from around the world.**

## About This Fork

**This is a modified fork of the Firestorm viewer that allows users to leave any joined experience, including privileged Second Life experiences.**

In the standard Firestorm viewer, users cannot leave or manage privileged experiences once joined. This fork removes that restriction by making minimal changes to the experience profile interface code, giving users full control over their experience participation.

### What's Changed

- Users can now click "Forget" to leave privileged experiences
- Users can block privileged experiences if desired
- The "Privileged" label still displays for transparency
- Only 2 conditional checks removed in `llfloaterexperienceprofile.cpp`

This demonstrates that the restriction is purely client-side UI logic and can be removed to respect user autonomy.

---

This repository is based on the official Firestorm viewer source code.

## Open Source

Firestorm is a third party viewer derived from the official [Second Life](https://github.com/secondlife/viewer) client. The client codebase has been open source since 2007 and is available under the LGPL license.

## Download

Pre-built versions of the viewer releases for Windows, Mac and Linux can be downloaded from the [official website](https://www.firestormviewer.org/choose-your-platform/).

## Build Instructions

Build instructions for each operating system can be found using the links below and in the official [wiki](https://wiki.firestormviewer.org).

- [Windows](doc/building_windows.md)
- [Mac](doc/building_macos.md)
- [Linux](doc/building_linux.md)

> [!NOTE]
> We do not provide support for compiling the viewer or issues resulting from using a self-compiled viewer. However, there is a self-compilers group within Second Life that can be joined to ask questions related to compiling the viewer: [Firestorm Self Compilers](https://tinyurl.com/firestorm-self-compilers)

## Contribute

Help make Firestorm better! You can get involved with improvements by filing bugs and suggesting enhancements via [JIRA](https://jira.firestormviewer.org) or [creating pull requests](CONTRIBUTING.md).

## Community respect

This section is guided by the [TPV Policy](https://secondlife.com/corporate/third-party-viewers) and the [Second Life Code of Conduct](https://github.com/secondlife/viewer?tab=coc-ov-file).

Firestorm code is made available during ongoing development, with the **master** branch representing the current nightly build. Developers and self-compilers are encouraged to work on their own forks and contribute back via pull requests, as detailed in the [contributing guide](CONTRIBUTING.md).

If you intend to use our code for your own viewer beyond personal use, please only use code from official release branches (for example, `Firestorm_7.1.13`), rather than from pre-release/preview or nightly builds.
