# The Soapstorm layer

Soapstorm fork features and conventions. Custom code is tagged `<SS:Nexii>` in shared files.

## Features

- **VATs Animation Editor** (`indra/newview/fsvatshost.*`, `fsvatseditor.*`) — In-viewer animation editor.
- **OTS Camera & Combat System** (`indra/newview/fscombathitmarker.*`, `llagentcamera.cpp`) — Over-the-shoulder camera, combat hitmarkers, weapon convergence.
- **Region Object Cache (ROC)** (`indra/newview/llvocache.*`, `doc/region_object_cache.md`) — Disk cache for static region objects.
- **Squeeze BC7 Texture Compression** (`doc/super_compressed_textures.md`) — Enhanced texture compression pipelines.
- **Alpha Mask Discard Optimization** (`doc/alpha_mask_verdict.md`) — Shader discard optimizations for alpha mask textures.
- **Sound Emitter Blacklist** (`indra/newview/fssoundemitterblacklist.*`) — Sound suppression and blacklist controls.
- **PBR Master Controls** — Controls for toggling and tuning PBR materials and fallbacks.
- **Transparent Avatar Snapshots** (`indra/newview/llsnapshotmodel.*`) — Alpha matte and transparent background capture for avatar renders.

## Note on Atmo Magic

The experimental Atmo Magic weather system (`ss*` files, weather simulation, wind flowmap, volumetric clouds, rain shadows, and surface fields) was removed on branch `refactor/remove-atmo-magic`.
