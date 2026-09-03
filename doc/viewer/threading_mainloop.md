# Main loop and threading

## Frame flow

Entry: `llappviewerwin32.cpp:638` `while (!viewer_app_ptr->frame())` → `LLAppViewer::doFrame()` (llappviewer.cpp:1580):

1. Perf auto-tune apply, trace bookkeeping (incl. cross-thread stat merge `pullFromChildren`, :1655).
2. `processMiscNativeEvents` + `gatherInput`.
3. **`mainloop.post(newFrame)` + `llcoro::suspend()`** (:1703-1711) — THE coroutine scheduling point; all HTTP coroutine completions land here (`HttpRequestPumper` on the mainloop pump, llcorehttputil.cpp:695).
4. `idle()` (:1760) → `display()` (:1792) → reflection map update.
5. Sleep/yield block (:1822-1943): yield settings, background throttle (40ms + texture cache pause), `updateTextureThreads` (pumps cache/decode/fetch reply queues), `LLLFSThread::updateClass`, `gMeshRepo.update()`, FPS limiter.

`pauseMainloopTimeout()` is held across `idle()` because messages can stall 20s+ (:1754 TODO) — the watchdog is blind to main-thread stalls there.

## idle() (llappviewer.cpp:5897) — ordered, all main thread

Timers → `LLGLTFMaterialList::flushUpdates` → **`gMainloopWork.runFor(MainWorkTime)`** (:5938 — the single throttle for ALL worker-thread reply callbacks) → agent update send → `idleNetwork()` (:6096, see [network_login.md](network_login.md)) → `gIdleCallbacks.callFunctions()` (:6122, **unbudgeted** — inventory background fetch, appearance mgr, material mgr, attachments all run here) → inventory/avatar-tracker observer notifies → `gViewerWindow->updateUI()` (:6151, full hover/tooltip pass) → selection/gestures/agent position → **`gObjectList.update()`** (:6210, per-active-object `idleUpdate` + flexi + tex-anim) → dead object cleanup → `LLWorld` visibilities/regions → **SS hooks: `SSWindFlowMap::sample()`, `SSAtmoMagic::wind()`** (:6293) → **`gPipeline.updateMove()`** (:6324) → particles → camera → **`gObjectList.updateApparentAngles()`** (:6357, amortized LOD pass) → audio.

## Threads that exist

| Thread | Where |
|---|---|
| "General" ThreadPool ×3 | llappviewer.cpp:2613 |
| "ImageDecode" pool, clamp(cores-4, 2, 8), `FSImageDecodeThreads` override | llappviewer.cpp:2649; llimageworker.cpp:70 |
| `LLTextureCache`, `LLTextureFetch` (LLWorkerThreads) | llappviewer.cpp:2660-2661 |
| `LLLFSThread` (file I/O), `LLPurgeDiskCacheThread` | :2625, :2668 |
| Mesh: `LLMeshRepoThread` + `LLPhysicsDecomp` + "MeshLodProcessing" ×2 | llmeshrepository.cpp:4336, :984 |
| `LLWindowWin32Thread` (OS message loop, width 1) | llwindowwin32.cpp:527 |
| **`LLImageGLThread`** (width 1, shared GL context) | llimagegl.cpp:2694; gated `RenderGLMultiThreadedTextures` (default OFF), needs GL>3.95 |
| **SSGLReadback** (width 1, shared GL context; `SSGLReadbackWorker`) | ssglreadback.cpp — generic texture→CPU `glGetTexImage` worker; rainshadow tiles + world-field bands read depth off it |
| llcorehttp `HttpService` (curl) | _httpservice.cpp:199 |
| Media plugin poll, CEF cache purge, pickers, watchdog, chat log, model loader, WebRTC | various |
| VBO worker `LLGLWorkerThread` — **compiled out** (`ENABLE_GL_WORK_QUEUE 0`) | llvertexbuffer.cpp:74 |
| SS: `SSSoundMeta` raw std::thread pool (PCM analysis) | sssoundmeta.cpp:50 |

## Cross-thread machinery

- `WorkQueue gMainloopWork("mainloop")` (llappviewer.cpp:415), drained under the `MainWorkTime` budget. `postTo(target, work, followup)` runs work on target and posts followup back to origin (workqueue.h:121).
- Coroutines: `LLCoros::launch` (158 call sites), rethrown into main each frame (:1711). `LLCoprocedureManager` named pools (5 coros each).
- Precedent for offloading CPU work: **`SSWindFlowMap::postWorker()`** (sswindflow.cpp:1525) — `main->postTo(general, work, followup)` with a generation counter to abandon stale results, GL halves kept on main, inline fallback if queues absent. Copy this pattern.
- Precedent for offloading GL readbacks: **`SSGLReadback`** (ssglreadback.cpp) — a generic 1-wide shared-context thread that guards the caller's writes with a `glFenceSync` the worker `glWaitSync`s before `glGetTexImage`, then posts the packed texels back to the mainloop queue. Rain shadow tiles (`ssrainshadow.cpp:221`) and world-field bands (`ssworldfield.cpp:529`) now read their depth off it; the wind flow solve+readback uses its own `SSWindFlowGLWorker` (sswindflow.cpp:117). The texture must survive, and must not be re-rendered, until the completion callback runs — the capture pipelines serialize with a pending flag.
- Mesh repo: 6 mutexes with documented lock order (llmeshrepository.h:447); main thread uses trylock-and-skip.

## Main-thread heavy spots (offload/optimization candidates)

1. **Occlusion readback**: `checkOcclusion` (llvieweroctree.cpp:1107) polls per group; on timeout (4 frames) does a **blocking** `glGetQueryObjectuiv(GL_QUERY_RESULT)` — pings the watchdog first because it knows it can stall.
2. `gObjectList.update` — linear active-object walk + `idleUpdate` each.
3. Rigged mesh/avatar skinning: `updateRiggedVolume`, `initSkinningMatrixPalette`, `updateRiggingInfo` — pure math, prime offload candidates.
4. UDP message decode (see network doc) — synchronous, allocation-heavy.
5. `gIdleCallbacks` fan-out is unbudgeted; texture loaded-callbacks loop is also outside budget.
6. `gMeshRepo.notifyLoadedMeshes()` runs inside `LLPipeline::rebuildPriorityGroups` (pipeline.cpp:3013) — mesh integration is charged to the render path.
7. GL texture uploads on main thread by default (see [textures_assets_cache.md](textures_assets_cache.md)).
8. Inventory gzip load during login is synchronous on main.
