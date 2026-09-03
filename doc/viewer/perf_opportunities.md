# Performance opportunities and simplifications

Synthesis of the per-system docs, ranked roughly by payoff/effort. Each item cites the evidence doc.

## VRAM / textures

1. **GPU-compressed textures** — today every texture uploads as raw RGBA (`RenderCompressTextures` default 0, format table stops at S3TC/DXT). Options, in escalating order: (a) flip on runtime DXT compression and fix quality issues; (b) transcode J2C → BC7 (quality) / BC1-BC5 at decode time on the ImageDecode pool and cache the compressed payload; (c) cache **GPU-ready compressed blobs on disk** so reload = fread → `glCompressedTexImage2D`, skipping J2C entirely after first decode. (c) also solves most decode cost and small-file churn for warm textures. ~4-8× VRAM and upload-bandwidth win. Evidence: [textures_assets_cache.md](textures_assets_cache.md).
2. **Async GL uploads**: enable/fix `RenderGLMultiThreadedTextures` (the shared-context `LLImageGLThread` already exists) or add PBO streaming; uploads are currently blocking client-pointer `glTexImage2D` on main.
3. **Fix VRAM accounting** — controller admits ~2× undercount (llviewertexture.cpp:526); the discard-bias logic steers on a bad signal, and the low-memory transition triggers a full-texture-list sweep hitch.

## Disk cache

4. **Consolidate the two one-file-per-asset caches** into a packed store (few large files + index, e.g. content-addressed slabs or LMDB-style). Kills: 80K+ small files, the 60s full-directory-scan purge thread, per-read `fopen/stat/mtime-rewrite`, and 16-way-sharded texture bodies. The texture cache's entries/cache/bodies triple plus a separate fast-cache is 4 formats to replace with one.
5. Make `LLFileSystem` reads async or route through `LLLFSThread` consistently — vorbis decode and fast-cache loads currently do blocking reads (fast-cache on the MAIN thread, with an acknowledged multi-second-stall comment).

## Main thread

6. **Offload skinning/rigging math** (`updateRiggedVolume`, skinning palettes) — pure math, no GL. Use the `SSWindFlowMap::postWorker` pattern (postTo General + generation counter).
7. **Occlusion query readback**: never block (`GL_QUERY_RESULT` on timeout path); carry results a frame later instead.
8. Budget `gIdleCallbacks` and texture loaded-callbacks (both currently unbounded per frame).
9. **UDP message decode**: the template decoder heap-allocates per block and copies every field, then handlers copy again — a pool allocator + in-place field views would cut most of it; or decode on a net thread and hand parsed structs to main. Object-update processing adds a third copy (2KB stack buffer per object).

## Network / login

10. Login inventory skeleton: gunzip + LLSD parse is synchronous on main — move to General pool.
11. Relogin: `reset_login()` (relog) and `disconnectViewer()` (quit) overlap but differ; unifying teardown into one audited path would make disconnect→relogin robust (currently fragile, one-shot `gDisconnected` latch).

## UI — Electron/Tauri feasibility (see [ui_system.md](ui_system.md))

Verdict: **plausible as an overlay/companion, not a wholesale replacement.**
- Plumbing exists: **LLLeap** spawns a child process speaking length-prefixed LLSD over stdio; 21 `LLEventAPI` listeners already expose floaters, UI callbacks, notifications, agent, inventory, settings, and synthetic input. A thin websocket↔LLEventPump shim (or speaking the LLLeap stdio protocol directly from the wrapper) is the missing piece — no websocket code exists in-tree.
- Cleanly externalizable: chat/IM, inventory (clean model/observer split — serialize `LLInventoryModel` + `changed(mask)` deltas), preferences, profiles, notifications, ss authoring floaters. These are exactly the "detached window panels" candidates.
- Must stay native: pie menus, HUD attachments, name tags, the lltool* build/edit family, drag-and-drop between inventory and world.
- The 2D UI already composites via the `mUIScreen` FBO — an external renderer could substitute that texture for in-viewport overlay if desired.
- Incremental path: keep XUI running, ship one floater (e.g. chat) as an external panel over LLLeap, grow coverage; avoid a big-bang port.

## Simplifications

- Dead code to ignore/remove: UDP texture fetch path (OpenSim legacy, dead on SL), `ENABLE_GL_WORK_QUEUE` VBO worker (compiled out), legacy UDP xfer asset path mostly bypassed.
- Duplicated probe-occlusion block in `doOcclusion` (pipeline.cpp:2817-2856 — verbatim copy for hero probes).
- `setSkipRenderFlag` asymmetry between deferred and post-deferred loops (pipeline.cpp:4349 vs 4503).
- Two J2C backends vendored (OpenJPEG default, KDU only proprietary builds) — one config to care about: OpenJPEG 2.5.3.
