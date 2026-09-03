# Viewer architecture docs

Deconstruction of how the Soapstorm viewer (Firestorm-derived SL viewer) works, written to steer agent work on rendering changes, performance, and UI. File:line refs verified 2026-08-26; treat as pointers, not gospel — code wins.

## Docs

| Doc | Covers |
|---|---|
| [frame_pipeline.md](frame_pipeline.md) | display() frame order, deferred pipeline, render targets + channel semantics, draw pools, culling/occlusion, SS hook points |
| [glow_and_alpha.md](glow_and_alpha.md) | **Read before ANY rendering change.** What alpha means per target/stage, the glow data path, blend/colormask rules, historical breakages |
| [ui_system.md](ui_system.md) | LLView/floater framework, XUI XML→widgets, input routing, LLEventAPI/LLLeap external-process bridge, Electron/Tauri boundary analysis |
| [textures_assets_cache.md](textures_assets_cache.md) | Texture fetch/decode/upload lifecycle, VRAM management, the two disk caches, mesh/sound/inventory/object caches, ranked bottlenecks |
| [threading_mainloop.md](threading_mainloop.md) | doFrame/idle ordering, every thread, WorkQueue/coroutine machinery, main-thread heavy spots |
| [network_login.md](network_login.md) | UDP message system + decode cost, HTTP/caps/event poll, login STATE machine, disconnect/relogin, object updates |
| [soapstorm_layer.md](soapstorm_layer.md) | The ss* systems, SS_ATMO shader variant rule, [interaction:] tags, fork conventions |
| [perf_opportunities.md](perf_opportunities.md) | Ranked improvement list: compressed GPU textures, cache consolidation, off-main-thread work, external UI feasibility |

Related, outside this folder: `doc/render_glow_pipeline.md` (verified glow trace), `doc/atmo_magic_*.md` (live Atmo docs), `doc/archive/` (out-of-date design/build logs — intent only).

## How it fits together (one frame)

Main thread does nearly everything. `doFrame()`: pump coroutines (all HTTP completions land here) → `idle()` (worker replies under `MainWorkTime` budget → **UDP message decode + handlers inline** → idle callbacks → UI update → object updates → SS wind/atmo → LOD) → `display()` (cull → sun shadows → **SS weather captures** → G-buffer fill w/ mid-loop occlusion queries → deferred lighting incl. SS wet pass + lightning lights → post-deferred alpha/water/**SS weather draw** → tonemap → glow → DoF/AA → UI) → sleep block (pump texture threads, mesh repo, FPS cap).

Worker threads: texture cache/fetch/decode, mesh repo, curl, file I/O, window messages, optional shared-GL upload thread. Everything reports back through WorkQueues drained on main.

## Key takeaways

1. **Screen alpha is the bloom mask, not coverage** — from deferred soften until tonemap, whatever lands in `mRT->screen.a` becomes glow (and shifts auto-exposure). Post-deferred default is alpha-writes-OFF; you opt in. Every "accidental glowing object" bug is a violation of this. See glow_and_alpha.md rules of engagement.
2. **GL state is manually managed and leaks by convention** — passes trample blend/colormask and only partially restore (doAtmospherics, weather block, wet pass draw-buffers). When adding a pass: name your target, blend func, and colormask; restore what the ambient state expects.
3. **The main thread is the bottleneck by design** — message decode, GL uploads, skinning, occlusion readback stalls, unbudgeted idle callbacks. Off-thread precedent to copy: `SSWindFlowMap::postWorker` (postTo General + generation counter, GL stays on main).
4. **Caches are one-file-per-asset, twice** — legacy texture cache + LLDiskCache, with a 60s full-scan purge thread. Textures upload raw RGBA; compression is off. This pairing (packed cache + GPU-compressed blobs) is the biggest cold-start/VRAM lever.
5. **UI is immediate-mode XUI, but an external-process bridge already exists** (LLLeap + 21 LLEventAPI listeners) — external panels are feasible without touching the widget framework.
6. **Fork discipline**: ss* files, `<SS:Nexii>` tags, `SS_ATMO` ifdef for shared shaders, `[interaction:]` tags for blast radius. Stock code stays byte-pristine when Atmo is off.

## Things to watch out for

- Magenta pixels = G-buffer clear showing through (nothing wrote there).
- G-buffer att0 alpha is the fullbright/emissive mix, NOT coverage — use discard for cutouts.
- Pool enum order is render order AND the occlusion-query insertion point; depth is shared between deferredScreen and screen.
- `postBuild()` returning false silently deletes a floater; `getChild` on a missing name returns a dummy widget, not null.
- Fetch state enum order is load-bearing (`mState > CACHE_POST` comparisons).
- Some messages arrive via event poll, not UDP — never assume the channel; the event-poll LLSD copy is a known crash risk area.
- `reset_login()` (relog) and `disconnectViewer()` (quit) are overlapping-but-different teardowns.
- Occlusion timeout path does a blocking GPU query readback.
- Perlin/noise determinism matters to Atmo sims — don't reorder sampling.
