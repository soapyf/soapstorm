# Textures, assets, and disk caches

Firestorm-derived: **two independent disk caches coexist** — legacy `LLTextureCache` for textures, `LLDiskCache`/`LLFileSystem` for everything else.

## Texture lifecycle

Hierarchy: `LLGLTexture` → `LLViewerTexture` → `LLViewerFetchedTexture` → `LLViewerLODTexture` (llviewertexture.h). Fetch worker state machine (lltexturefetch.cpp:429, **enum order is load-bearing** — code compares `mState > CACHE_POST`):

INIT → LOAD_FROM_TEXTURE_CACHE (async read on TextureCache worker thread) → CACHE_POST → LOAD_FROM_NETWORK (cap URL `.../?texture_id=uuid`; UDP sim path is OpenSim-legacy, dead on SL) → WAIT_HTTP_RESOURCE (semaphore, ~40 high water) → SEND_HTTP_REQ (**byte-range GET** sized by discard math; `mRequestedOffset -= 1` Varnish hack :1627) → WAIT_HTTP_REQ → DECODE_IMAGE (posted to ImageDecode pool) → WRITE_TO_CACHE (also writes 16×16 "fast cache" thumb) → DONE.

- `doWork()` **holds `mWorkMutex` for the whole state machine** (:1146) — contends with main-thread `getRequestFinished`.
- J2C decode: **OpenJPEG 2.5.3 by default** (`-DUSE_KDU=FALSE` in autobuild.xml; KDU only in proprietary builds). Discard math in llimagej2c.cpp:269-408.
- Decode thread: `LLImageDecodeThread` is a façade over `LL::ThreadPool("ImageDecode", 8)` (llimageworker.cpp:70), width overridable via `ThreadPoolSizes`.
- **GL upload**: `scheduleCreateTexture` (llviewertexture.cpp:1668) posts to a second-GL-context thread ONLY if `RenderGLMultiThreadedTextures` (default **0**); otherwise blocking `glTexImage2D`/`glTexSubImage2D` with client pointers **on the main thread**. No PBO streaming (sScratchPBO is readback scratch only).

Per-frame: `updateImages` budget = clamp(5% of frame, 2-5ms) split three ways (fast-cache loads / fetch updates / create textures, 33% floor each); loaded-callbacks run **outside** any budget. Fetch update walks max(32, N/20) textures/frame, resuming from a cursor.

## VRAM management (`LLViewerTexture::updateClass`, llviewertexture.cpp:501)

Budget = VRAM / `RenderTextureVRAMDivisor` (default 2), floored 1GB. "Used" is estimated from GL texture + VBO byte counters — **the comment admits it misses ~half of real VRAM use**, so the bias controller steers on a bad signal. Discard bias slams to ≥1.5 on first low-memory frame; low-memory transition triggers a **full-list decode-priority sweep** (guaranteed hitch, :568); bias clamped [1,4], forced 5 when backgrounded. Downrez enqueues to `mDownScaleQueue`, drained via **FBO blits** in `updateImagesCreateTextures` — deliberately overrunning the frame budget (llviewertexturelist.cpp:1194-1211).

**Compression: effectively unused.** `RenderCompressTextures` defaults 0 (EXPERIMENTAL); format table covers only S3TC/DXT1/3/5 (+sRGB) — no BC5/BC7/ASTC. `glCompressedTexImage2D` used only for pre-compressed source. **Textures upload as raw RGBA — the single biggest VRAM lever available.**

## Disk caches

- **Texture cache** (lltexturecache.cpp): `texture.entries` (fixed entry array) + `texture.cache` (first ~1KB of each texture) + `textures/[0-F]/<uuid>.texture` bodies + fast-cache thumbs. Eviction purges 20% LRU-ish; lazy purge capped 4ms/slice. **One file per texture body, sharded only 16 ways → the small-files problem.**
- **LLDiskCache** (llfilesystem/lldiskcache.cpp): `sl_cache_<uuid>_0.asset`, one file per asset. Purge thread wakes **every 60s** and does a full recursive directory scan + mtime sort ("5 seconds+ ... to scan 80K+ items", :522). `LLFileSystem::read` is blocking stdio per call; every read stats + may rewrite mtime (throttled 1h).
- **Object cache**: `objects_%d_%d.slc` per region (llvocache.cpp:1178), max 128 regions, raw APR binary, no compression.
- **Inventory**: `<agent>.inv.llsd` gzipped; load = full synchronous gunzip + LLSD parse **on the main thread during login** (llinventorymodel.cpp:3007).

## Other assets

- **Mesh** (llmeshrepository.cpp): one `LLMeshRepoThread` (8ms work slices) + `ThreadPool("MeshLodProcessing", 2)` for cacheOptimize. Storage in LLDiskCache with in-place READ_WRITE patching of partial files. Five mutexes with documented lock order (:184); main thread uses `LLMutexTrylock` and skips on contention.
- **Sounds**: vorbis decode reads go through the blocking disk-cache path (llaudiodecodemgr.cpp:96).
- Legacy UDP xfer asset path still present (llassetstorage.cpp).

## Ranked bottlenecks (from code evidence)

1. Raw-RGBA uploads, compression off by default — VRAM + bandwidth.
2. Main-thread blocking GL uploads (`RenderGLMultiThreadedTextures`=0), no PBO streaming.
3. Fast-cache loads do synchronous APR reads on main thread under a mutex (mitigated by trylock+skip; comment concedes multi-second stalls on busy disk, llviewertexturelist.cpp:1241).
4. Downscale queue overruns frame budget with FBO blits.
5. Emergency full-texture-list sweep on low-memory transition.
6. `mWorkMutex` held across the whole fetch state machine.
7. Two one-file-per-asset disk caches; 60s full-scan purge thread.
8. VRAM accounting ~2× off → bias controller mis-steers.
