# Squeeze: BC7 texture tier, cache layout, and texture-manager verdict

Project code name **Squeeze** (owner, 2026-08-27) - the user-facing name of this work, and the label of its preferences tab under Preferences > Soapstorm > Squeeze. Settings are `SSSqueeze*` plus `SSBC7CacheSize`.

Status: ACCEPTED with owner amendments, 2026-08-26. Produced from a 13-agent design review (6 subsystem maps, 3 competing designs under opposed priors, adversarial critique of each, cross-design adjudication); ~110 file:line claims verified against the tree. Owner decisions folded in 2026-08-26: FULL-RES-ONLY BC7 with background promotion (supersedes encode-at-best-seen), lax watermark eviction, SSD-class hardware assumed, network promotion ON. The companion Region Object Cache design is settled in `doc/region_object_cache.md` (2026-08-27) — it feeds the region manifests below and shares the container discipline. Remaining OWNER CALLs are marked.

## Verdicts (TLDR)

1. **BC7 tier**: build it as an append-only sidecar store (index + capped data segments) beside the existing J2C texture cache. J2C stays source of truth; BC7 is derived, versioned, and wipeable. FULL-RES-ONLY by owner mandate: every BC7 record is discard 0 with a full mip chain; a background promotion engine completes partial J2C via low-priority network fetches and encodes from full res. First user-visible build ~5-7 weeks.
2. **Region/agent data files (layout c)**: REJECT as physical layout, ADOPT as *manifests* over the shared UUID-keyed store. All three designs independently reached this. Manifests deliver 100% of the wanted UX (teleport warm-up, agent-appearance warm-up, evict-by-last-visited/last-seen) and get shared-asset semantics *right* where physical per-region files cannot. Details below — this is not a consolation prize, it is the correct implementation of the idea.
3. **Texture manager rewrite**: NO from-scratch rewrite now. The premise is off — there is no single texture manager; priorities live in LLViewerTextureList, decode scheduling in LLTextureFetch + the ImageDecode pool, cache traffic in LLTextureCache, per-texture glue in LLViewerFetchedTexture. Every BC7 goal lands at existing verified seams. Adopt two cheap hedges now (ITextureStore interface seam; explicit BC7 residency state ladder) that keep a later rewrite cheap if telemetry ever justifies it.
4. **Work queue granularity**: stage-parallel on the demand path (LLPointer handoff of the decoded raw to a dedicated background-QoS BC7 pool), fused full-sequence items only for idle backfill. The "avoid moving memory" concern dissolves: LLImageRaw is refcounted, so handoff moves 8 bytes, not 4 MB, and a 4-16 MB raw doesn't fit any core's cache anyway. Never fuse encode into the ImageDecode pool (latency inversion by construction).
5. **Unified all-asset cache (the full new cache system)**: deferred behind explicit triggers, not scheduled. The BC7 sidecar IS the first index+big-datafile cache, scoped to one tier; its container discipline is written so the unified store can adopt it later. OWNER CALL #6 below.

## What exists today (verified)

- TWO disk caches: legacy LLTextureCache (texture.entries fixed-record index + texture.cache 600-byte-prefix pool + textures/[0-f]/UUID.texture bodies + FastCache 16x16 thumbs) — J2C only, one dedicated worker thread, sole consumer LLTextureFetch. LLDiskCache/LLFileSystem for everything else (one loose file per asset, mtime-LRU purge thread doing a full recursive scan every 60s — 5s+ at 80K files, lldiskcache.cpp:522). This fork kept the old texture cache upstream deleted; "textures use LLDiskCache" is false here.
- Fetch is progressive: byte-range GETs sized by discard math; full resolution is the EXCEPTION (discard 0 needs ~full-screen coverage or boost). mFileSize=datasize+1 is the in-band partial sentinel (lltexturefetch.cpp:2078-2088). The fetch queue is FIFO — the priority set in llqueuedthread is commented out; priority docs at lltexturefetch.cpp:121-135 are stale.
- Uploads are raw RGBA on the main thread by default (RenderGLMultiThreadedTextures=0). Compression is effectively unused; format tables are DXT-only; no BPTC anywhere in-tree (glext.h has the enums). This is the single biggest VRAM lever available.
- Compressed upload contract: mip chain stored smallest-first, walked backward with per-mip glCompressedTexImage2D (llimagegl.cpp:779-801); compressed+mips without data_hasmips is fatal LL_ERRS (llimagegl.cpp:991); the compressed branch bypasses analyzeAlpha/updatePickMask (813/815).
- Derived-artifact precedents exist: FastCache 16x16 thumbs (the structural template for BC7 — derived-from-decode, sidecar, version-wiped) and decoded Vorbis→WAV .dsf files.
- VOCache is the per-region-file precedent: objects_X_Y.slc works per-region ONLY because its payload (local_id+CRC ObjectUpdate blobs) is meaningless outside its region. Asset bytes are the opposite: globally UUID-keyed and heavily shared.
- Other avatars: baked textures arrive with per-appearance-state UUIDs (churn on outfit change; disk-cached by UUID today, which pays off on unchanged re-sighting). Attachment mesh/texture UUIDs are the stable cross-sighting payload and arrive only via object streaming. AttachmentBlock carries inventory item IDs, not asset IDs.
- No server manifest of a region's assets exists (no cap, nothing in SimulatorFeatures) — but for revisited regions, loadObjectCache at handshake (llviewerregion.cpp:3427) materializes every previously-seen object's DP blob containing all texture/sculpt/mesh UUIDs, which the current code never mines. That is a free, locally-owned region manifest.

## BC7 sidecar architecture

LOCATION: `<cache>/texturecache/bc7cache/` — inside the texture cache dir so old viewers sharing the cache never see it, and wipes can cascade (but see purge-cascade fix below; the rename trick alone is NOT sufficient).

FILES: `bc7.idx` (append-only 64B records after a 64B header carrying format_version + encoder_version + j2c_source_version + address size) + `bc7_NNN.dat` append-only segments capped 512 MB. Record: uuid, segment/offset/size, full WxH, base discard, mip_count, format (BC7_UNORM now; BC7_SRGB/BC6H/BC5 reserved), flags (alpha_is_mask, fully_opaque, tombstone, has_pickmask, source_was_partial), **source-components byte** (see fix 2), U32 pickmask_bytes (U16 overflows at 2048²), last_use_day, data crc32, record crc32. Later record for same UUID supersedes; tombstone deletes. In-memory unordered_map built by one sequential read at startup.

BLOB: header + optional 1-bit pick mask + mip chain in the exact data_hasmips layout (smallest mip first), chain covering base_discard..5 so every upload always carries full mips (the LL_ERRS at llimagegl.cpp:991 makes miplessness fatal). One read → one buffer → glCompressedTexImage2D loop, zero reshuffle. Bonus: a prefix read yields a usable low-res chain (progressive property; eventually replaces FastCache).

CRASH SAFETY: append-only everywhere; blob before index record; per-record CRC; recovery = truncate index at last valid record, truncate segments to logical end computed from the index. Disk full → session read-only. Corruption → wipe bc7cache only (derived data; re-encode is the only cost). Versioning: wipe-on-mismatch, per the only precedent in the tree (llappviewer.cpp:5228-5246); encoder_version bump wipes only this tier.

TWO-LOCK DISCIPLINE (critique fix): map lock for lookups; store lock for file I/O; **no file I/O ever under the map lock** (multi-MB appends sharing the lookup mutex would stall the main thread). last_use touches from the main-thread lookup path are queued to the pool, never written inline.

MULTI-INSTANCE: reuse the exec-marker second-instance gate (llappviewer.cpp:4776-4811); instance 2 is a read-only snapshot, no encode pool, no eviction, no orphan sweep, no compaction. CORRECTION (2026-08-27, verified in the tree): the earlier claim that "readers open segments with FILE_SHARE_DELETE" was WRONG and is retracted. `LLFile::fopen` is `_wfopen` (llfile.cpp:375-382) and `LLFile::fsopen` is `_wfsopen` (:388-393); neither grants delete sharing, and LLFile has no `CreateFileW` entry point at all. A reader therefore blocks the writer's unlink for as long as it holds the handle. Eviction is built so that this does not matter: killing is an INDEX operation and the unlink is best-effort space reclaim, so a failed unlink is NORMAL and is never rolled back — the records are already dead, the file becomes an orphan, and the startup sweep or the id's reuse takes it. What makes a stale (segment, offset) pair safe once ids can be reclaimed is not share-delete, it is that `ssBC7VerifyBlob` takes the expected uuid as a REQUIRED parameter and the read lives inside the store, so no caller can be handed a raw offset and skip the check. A `CreateFileW` helper with `FILE_SHARE_DELETE` would only shrink the orphan window, not change the correctness argument.

EVICTION (Phase 3 — IMPLEMENTED 2026-08-27, policy in `indra/newview/ssbc7storeevict.cpp`): LAX WATERMARK MODEL (owner mandate): nothing is evicted until the store passes the SSBC7CacheSize budget. last_use / manifest last_seen is purely a drop-first priority order, never a TTL — a record is never dropped for being old. SSD-class disks assumed (owner: target is gaming desktops) — no HDD I/O-throttle engineering.

COPY-FORWARD IS RETRACTED. The mandate above said "copy-forward live blobs out of the oldest segment, then delete the segment whole". That is the right reflex for an LSM or a log-structured filesystem, where segments accumulate garbage from overwrites and copy-forward is how you harvest it. This store has no such garbage: the writer is a pure log and records are immutable per (uuid, encoder_version) under full-res-only, so a segment is exactly "the textures first seen during one window of time" and its liveness only falls when eviction itself decides to drop something. Copy-forward would therefore spend real read and write I/O on blobs it merely BELIEVES are hot. What replaces it is lazy re-encode: `ssBC7EncodeConsider` already fires on every full-res DONE decode and is suppressed only by `hasRecord()`, so a dropped record re-encodes automatically on a decode the viewer was doing anyway, and lands in the youngest segment — generational promotion with no tenure flag and no persisted state. Re-encode IS the copy phase, performed lazily and only for what demand proves hot. `mReEncodedAfterEvict` in the store metrics is the number that decides whether that bet is right: if re-encoded bytes ever run past roughly a sixth of bytes written, copy-forward or an explicit tenured generation is worth revisiting.

MECHANISM. One pass kills at most one whole `bc7_NNN.dat`, and blob bytes are never read, moved or rewritten. Rank every live record store-wide by drop priority (last_use_day, then an in-memory session tick) and take the coldest ~64 MB as a GLOBAL DOOM SET; score each sealed segment by how much of that doom set it holds, tie-broken by size; exclude the write head, exclude any segment already dying, exclude any segment more than 25% referenced in the last 120 seconds; tombstone every record in the victim as ONE contiguous index write plus one `fflush`; re-scan and refuse to unlink if anything live still points at it (verdict STILL_REFERENCED); unlink outside every lock. Global-rank-first rather than per-segment scoring is what makes a hot record stranded in a cold segment visible in the log instead of invisible.

THE ORDERING INVARIANT: tombstone, flush, and only then touch the file. At every instant a record is either live with its bytes intact or dead with its bytes reclaimable, never both. Exactly one durability point per kill, a valid store on both sides of it, no `fsync` (every crash ordering collapses to the same read-side uuid and checksum check, so the extra barrier would buy nothing but SSD wear), and nothing is ever truncated.

NOTHING ABOUT EVICTION IS PERSISTED. The free list, the per-segment frontiers, the byte totals and the write head are all derived from one index scan plus one directory listing, so there is no second file to fall out of step with bc7.idx and therefore no torn cross-file state to reason about.

THE HEAT SIGNAL. `mLastUseDay` on its own is creation-order FIFO, which on a first session drops the home region first — because the home region is what was encoded first. Two things fix that and both shipped in the same change. `hasRecord()` stamps the day, a session tick and a wall-clock second on every hit; it is the encode gate's dedupe check, so it fires on every full-res decode that reaches DONE, including plain J2C cache hits. And a once-a-minute main-thread pass marks every uuid `LLViewerTextureList` still holds, without which permanently resident textures (avatar skins, the UI atlas, system assets) decode once at login and then rank as the coldest things in the store, which is exactly backwards. The tick and the second are in-memory only; losing them to a crash costs ordering quality and nothing else, which is the whole latitude a drop-priority signal has that a TTL would not. The hard 120-second recency floor is deliberately independent of `mLastUseDay`, because a U16 of days cannot express "a minute ago" and a minute ago is precisely where dropping a record is most visibly wrong.

SEGMENT CAP: `SSBC7_SEGMENT_CAP` (512 MB) is a FORMAT CEILING and must never move — `deserializeRecord` rejects any record whose blob ends past it and `loadIndex` stops at the first rejected record, so lowering it would not reject one record, it would silently discard the entire tail of an index written by an older build. The cap the writer actually rolls over at is a separate runtime value, `clamp(budget / 24, 16 MB, 512 MB)`, which is what gives segment-granular reclaim enough buckets to be selective at the 512 MB low end of the slider — one 512 MB segment would be the entire store in a single bucket.

SEGMENT ID REUSE IS MANDATORY, NOT A REFINEMENT. Ids are consumed at the rate the store is WRITTEN, not the rate it grows; at the observed write rate a monotonic allocator burns the 4096-id space in about a day of exploring, after which `appendBlob` refuses for the rest of the session with only a warning. Freed ids go on a derived free list, allocated lowest-first, never persisted. `ensureSegment` rewrites the header of a reused file, because a reused id whose file survived a failed unlink is not empty — which is safe only because every read checks the blob's uuid.

THE HARD CAP. Past budget + 2 segments, `append` refuses with an explicit reason and counts it. A design that can decline to evict must not also be allowed to grow forever.

EVERY REJECTION CARRIES A REASON (`ESSBC7EvictVerdict`), counted and folded into `metricsString()`, so "eviction is off" and "eviction ran and declined everything" are never the same log line: RAN, NOT_NEEDED, DISABLED, READ_ONLY, NOT_READY, STALE, ALREADY_RUNNING, COOLDOWN, BUDGET_INVALID, NO_CANDIDATE, ALL_HOT, STILL_REFERENCED, INDEX_WRITE_FAILED, UNLINK_FAILED.

WHERE IT RUNS. On the existing BC7Encode pool via `tryPost`, gated by an in-progress flag the pass itself owns, so a refused post is not an error and simply retries at the next tick. No second thread pool. The rollover trigger inside `appendBlob` sets a flag rather than posting, because it runs under the store lock that the posted work would itself want. Main-thread cost in the steady state is one clock comparison per frame and, once a minute, one walk of the resident texture set.

STARTUP ONLY, inside `initStore` before the store is published — no readers, no writers, no lock ordering question: the orphan sweep (any `bc7_*.dat` no live record points into, with the filename round-tripped through `segmentPath` and an exact string match required, refusing outright unless the directory's last component is `bc7cache`), then the budget trim (the one moment where a budget the user lowered between sessions gets honoured without pushing I/O into a live session), then index compaction (tmp file plus `LLFile::rename`, when more than half the index is dead, scrubbing records whose blobs no longer fit their segment while the bytes are already in hand).

KNOWN BEHAVIOUR THE PREFERENCES UI SHOULD BE HONEST ABOUT: the store CAN sit above the slider value, by up to two segments, for as long as every sealed segment is over the hot bar. The copy should say "approximately" rather than implying a hard limit.

## Encode pipeline

HOOK: LLTextureFetchWorker DONE state (lltexturefetch.cpp:2128-2146), NOT WRITE_TO_CACHE — DONE also covers cache-hit decodes, and the loop-back-to-INIT condition sits right there, enabling skip-if-looping so intermediate discards streaming in are never encoded. The work item carries LLPointer<LLImageRaw> (refcounted; encoder treats it as strictly immutable and takes LLImageDataSharedLock), UUID, decoded discard, dims, components, mHaveAllData.

FULL-RES-ONLY (owner mandate, supersedes the review's encode-at-best-seen): encoding partial-res sources is wasted work and space, because the full-res asset will be visible at some point — so BC7 records exist ONLY at discard 0. The demand-path hook encodes immediately only in the free case (mHaveAllData && decoded at discard 0); every other decoded texture just lands its UUID on the promotion want-list. THE PROMOTION ENGINE (now core, not optional) drains that list on the BC7 pool in strict preference order: (a) J2C already COMPLETE in cache but lacking BC7 → fused [read → decode → mips → encode → store], zero network; (b) no complete-J2C work remaining → issue low-priority continuation fetches (offset=have, the fetch pipeline's native mode) to COMPLETE partial J2C for want-listed textures, then encode on arrival. Network completion is throttled (bandwidth setting, idle-only, paused during teleport/login) and targeted — only textures referenced by a manifest or with recent last_use, so one walk through a busy mall does not queue thousands of full-res fetches for textures never seen again.

SIMPLIFICATIONS this buys: records are effectively immutable per (UUID, encoder_version) — no supersede churn, no re-encode debounce, and dedupe is by plain UUID again. SERVING: every BC7 hit satisfies ANY desired discard. The smallest-mips-first blob layout means mips d..5 are a contiguous PREFIX of the blob — serving desired discard d is a prefix read + upload starting at that mip, so VRAM cost still tracks demand even though storage is always full-res. VRAM-pressure down-rez = re-upload a shorter prefix (RAM-resident tail mips make this free below 128px).

MIPS: generated at encode time (box filter, LLImageBase::generateMip precedent) — required, since glGenerateMipmap is illegal on compressed. Parity of sRGB-space box filtering + alpha at small mips vs today's GPU mips needs a visual check on cutout content (open question).

FORMAT: GL_COMPRESSED_RGBA_BPTC_UNORM (0x8E8C), NOT the SRGB variant — zero GL_SRGB internal formats are used for world textures; shaders do srgb_to_linear, so UNORM preserves sampling semantics exactly. gGLManager.mHasBPTC runtime check (core GL 4.2), silent RGBA8 fallback.

ENCODER: two interchangeable backends behind one seam, `ssBC7EncodeBlocksRGBA(num_blocks, rgba_blocks, out_blocks, quality)` in `indra/llimage/ssbc7encoder.h`. The seam is batched, not per-block, because a SIMD encoder's whole advantage is filling vector lanes with independent blocks; the wrapper hands over one row of blocks per call. Which backend is compiled is decided at configure time by the `SS_BC7F` option in `indra/llimage/CMakeLists.txt` — see "Encoder backend" below. The P0 measurement gate is now closed; numbers are in that section.

EXCLUSIONS (enqueue + read side): sculpt (geometry-not-color, FIRE-35428), media, UI/local/explicit-format, mNeedsAux, FTT_SERVER_BAKE in v1 (bake UUIDs churn per outfit; OWNER CALL #2), icon/thumbnail variants — keyed by BOOST_ICON/BOOST_THUMBNAIL boost level, not just ETexListType (the destructive rescale keys off boost). Raw-consuming textures (needsToSaveRawImage etc.) always take the J2C path. NPOT sources skipped.

## Encoder backend: measurement, choice, and licensing

Two backends implement the block seam. Exactly one is ever compiled, because both define the same
functions; `indra/llimage/CMakeLists.txt` swaps them on the `SS_BC7F` option.

- `ssbc7block_bc7f.cpp` — the DEFAULT since 2026-09-09: a thin shim onto Binomial's bc7f, the
  analytical one-shot BC7 encoder inside the Basis Universal transcoder, vendored unmodified in
  `indra/llimage/basis_universal/` (see the README there). Plain C++, no SIMD, no toolchain beyond
  the compiler, so every platform and every CI build gets it. Rungs are bc7f's own presets:
  FAST = `Faster`, BALANCED = `Fast`, HIGH = `Default`, the owner's pick.
- `ssbc7block_mode6.cpp` — entirely our own code, no third-party dependency. Implements BC7 mode 6
  only: single partition, 7-bit endpoints plus a shared p-bit, 4-bit indices. Built by
  `SS_BC7F=OFF`; it exists as the structural licensing hedge described below.

**History.** From 2026-08-27 to 2026-09-09 the default was Binomial's bc7e, compiled by Intel ISPC
when `SS_ISPC_EXECUTABLE` was configured, with mode 6 as the fallback. It was dropped, together with
the ISPC dependency and its licence entries, when bc7f was measured (next section but one) and the
owner chose bc7f Default: it beats the portable backend on every content class at 10-15x the
speed, and unlike bc7e it needs no toolchain, so the CI builds - which never had ISPC and were
shipping mode 6 all along - now ship the same encoder the owner runs. The bc7e numbers below are
kept as the record they were.

### Measured, not assumed

512x512 synthetic content standing in for real texture classes, single threaded, decoded by
`bc7decomp.c` from the bc7e distribution — a decoder written by someone other than us, so a shared
misreading of the BC7 bit layout cannot cancel out and report a false pass. PSNR in dB, higher is
better; 99.00 means bit-exact. Stable to 0.01 dB and within 2% throughput across runs.

| backend | smooth | skin/low sat | alpha cutout | two-tone | 4 hues per block | noise | Mpix/s |
|---|---|---|---|---|---|---|---|
| ss-mode6-lsq   | 54.23 | 52.96 | 63.43 | 54.15 | 11.12 | 15.09 | 13.2 |
| bc7e ultrafast | 54.77 | 54.12 | 99.00 | 54.15 | 11.13 | 15.03 | 73.5 |
| bc7e veryfast  | 55.03 | 55.53 | 99.00 | 54.15 | 54.15 | 17.96 |  9.1 |
| bc7e fast      | 55.04 | 55.88 | 99.00 | 54.15 | 54.15 | 17.15 |  7.9 |
| bc7e basic     | 55.39 | 57.33 | 99.00 | 54.15 | 54.15 | 18.91 |  2.5 |
| bc7e slow      | 55.11 | 56.69 | 99.00 | 54.15 | 99.00 | 19.00 |  3.4 |

Reading it:

- **The "4 hues per block" column is the whole story.** It is four unrelated saturated hues inside
  every 4x4, which no single line through RGBA space can pass near — the exact thing the partitioned
  modes exist for, and the one weakness of a single-mode encoder. ultrafast shares that weakness;
  veryfast fixes it by 43 dB. That step is a different kind of improvement from the ones after it:
  it removes a failure mode rather than sharpening an already-good result.
- Everything past veryfast is ordinary diminishing returns — basic buys 1.8 dB on low-saturation
  content for 3.6x the time. The three exposed rungs are FAST (ultrafast), BALANCED (veryfast) and
  HIGH (slow); `basic` is deliberately not on the ladder, because `slow` DOMINATES it — repeatably
  faster, and bit-exact on the multi-hue case `basic` merely makes acceptable, for six tenths of a
  decibel on smooth content. The ladder is built from this table and not from the profile names.
- "two-tone edges" is identical across every backend, which is the useful negative result: a 4x4
  containing only two colours fits mode 6 *exactly*, endpoints landing on the colours themselves.
  A hard edge is not what breaks a single-mode encoder; several unrelated colours is.
- `basic` being slower than `slow` is real and repeatable, not noise. Unexplained, and not worth
  explaining — neither is the default.
- Noise at 15-19 dB across all backends is expected and irrelevant: noise is incompressible, and no
  real texture looks like it.

The portable backend's numbers are identical whether measured with our own mode-6 decoder or with
bc7decomp, which is independent confirmation that it writes correct BC7.

`ssBC7BlockBackendVersion` is a plain backend id (bc7e 100, the portable backend stays in the low
numbers) so a blob from one backend can never be mistaken for the other's. **Quality is NOT folded
in** — see "Adaptive encode quality" below for why that changed and what replaced it.

**Determinism is per machine, not universal.** bc7e dispatches on the host instruction set and is
built with fast maths, so another CPU may produce different bytes. This costs nothing: a blob is
only ever read back by the installation that wrote it, and what the version stamp promises is that
the same machine encoding the same bytes twice gets the same block. `ssbc7encoder.h` states this at
the seam so nobody later "fixes" it into a cross-machine guarantee we do not need and would pay for.

### bc7f and bc7e_scalar, measured 2026-09-09

Asked: should bc7e be "upgraded" to Binomial's bc7f, the analytical one-shot BC7 encoder that ships
inside the Basis Universal transcoder (`transcoder/basisu_transcoder.cpp`, namespace `basist::bc7f`,
v2.50 and later)? Measured rather than assumed. Harness: `V:\Scratch\squeeze` (`bench.cpp`,
`build.ps1`), same shape as the table above but DIFFERENT synthetic images, so compare rows within
this table only. Four encoders: bc7e via ISPC exactly as the viewer builds it, `bc7e_scalar`
(Binomial's pure C++ port of bc7e in `encoder/basisu_bc7e_scalar.cpp`, same API, same output to
0.01 dB), bc7f at its six upstream presets, and our portable mode 6 backend. Decoded by `bc7decomp`
and cross-checked block by block against Basis's own `bc7u::unpack_bc7`: every block agreed, and
every backend was byte-identical run to run. `alphasmooth` is new: smooth colour under a smooth
alpha ramp, because bc7f's own comments call its cheap presets "very weak particularly on alpha".

PSNR dB over RGBA, 512x512, single thread, 99.00 = bit exact:

| backend | smooth | skin | cutout | alphasmooth | twotone | fourhue | noise |
|---|---|---|---|---|---|---|---|
| ss mode6 (portable)   | 50.86 | 52.63 | 58.94 | 52.66 | 53.17 | 15.78 | 15.08 |
| bc7e ispc ultrafast   | 51.17 | 53.46 | 73.68 | 54.81 | 53.17 | 15.62 | 15.02 |
| bc7e ispc veryfast    | 54.12 | 53.68 | 73.68 | 54.27 | 53.24 | 43.15 | 17.96 |
| bc7e ispc slow        | 55.27 | 54.01 | 99.00 | 55.99 | 55.23 | 51.83 | 19.00 |
| bc7f Default          | 51.92 | 52.64 | 73.68 | 52.68 | 53.17 | 32.33 | 17.66 |
| bc7f PartAnalytical   | 51.97 | 52.67 | 73.68 | 53.56 | 53.17 | 34.22 | 18.45 |
| bc7f NonAnalytical    | 54.47 | 54.69 | 73.68 | 54.17 | 53.28 | 34.24 | 18.45 |

Mpix/s per class (the whole-image average is misleading because `cutout` is mostly solid blocks,
which every encoder short-circuits):

| backend | smooth | skin | cutout | alphasmooth | twotone | fourhue | noise |
|---|---|---|---|---|---|---|---|
| ss mode6 (portable)   |   6.5 |   6.2 |  69.1 |   6.5 |   6.5 |   6.3 |   6.1 |
| bc7e ispc ultrafast   |  71.2 |  72.0 |  28.8 |  11.0 |  69.6 |  71.0 |  70.4 |
| bc7e ispc veryfast    |   8.3 |   9.2 |  11.2 |   5.4 |   8.8 |   8.5 |   7.4 |
| bc7e ispc slow        |   2.7 |   2.9 |   6.6 |   3.6 |   2.7 |   2.7 |   2.4 |
| bc7e scalar slow      |   0.5 |   0.5 |  27.4 |   0.7 |   0.6 |   0.7 |   0.5 |
| bc7f Default          |  70.0 | 102.5 | 896.8 |  54.4 |  61.8 |  14.5 |  11.5 |
| bc7f PartAnalytical   |  35.2 |  76.4 | 792.2 |  28.2 |  46.6 |   8.0 |   6.5 |
| bc7f NonAnalytical    |   7.9 |   7.2 | 386.5 |  12.2 |   6.8 |   7.9 |   6.5 |

Reading it:

- **bc7f is a speed encoder, not a quality one.** Against the HIGH rung the viewer defaults to
  (bc7e slow), bc7f NonAnalytical is about 3x faster and gives up 0.8 dB on smooth, 1.8 on
  alphasmooth, 2 on twotone, bit-exactness on cutouts (73.7 vs 99.0) and 17.6 dB on the multi-hue
  case. Under this feature's economics — encode once, quality is permanent, the pool is supply
  limited — that is a regression, so **HIGH stays bc7e slow** and bc7f is not an upgrade there.
- **bc7f Default dominates the portable mode 6 backend on every column** at 10-15x the speed on
  textured content: +14.7 dB on cutouts, +16.5 dB on the multi-hue case, +2.6 dB on noise, never
  worse anywhere. It is the right thing for a build with no ISPC to fall back to, and it also
  removes ultrafast's multi-hue blind spot from the FAST rung (32 dB against 15.6) at a similar
  speed on textured content.
- **bc7e_scalar is not a substitute for ISPC.** Its output matches the ISPC build, but at `slow` it
  is 5x slower on every textured class. It is the most direct way to give a no-ISPC build the real
  bc7e ladder, but that costs the very throughput the ladder exists to buy.
- Cost of carrying bc7f: it cannot be lifted out of `basisu_transcoder.cpp` without modifying an
  upstream file, so the whole transcoder directory (4 MB of source, one 46k-line translation unit)
  would be vendored unmodified and built as its own component. That is cheaper than it sounds:
  about 4 s to compile at /O2 and a 2.7 MB object. `BASISD_SUPPORT_KTX2=0` and
  `BASISD_SUPPORT_KTX2_ZSTD=0` are the only trims that build; switching off every format except
  XUASTC (where bc7f is guarded) fails to compile upstream. The API is per block
  (`bc7f::fast_pack_bc7_auto_rgba`), portable, no SIMD, thread safe once
  `basisu_transcoder_init()` has run (wrap it in `std::call_once` like bc7e's init).
- Licence: identical to bc7e's. Basis Universal is Apache 2.0 from the same rights holder
  (Binomial LLC); its DEP5 file marks only `zstd/` and `encoder/3rdparty/` as third party, and
  neither is touched. The posture below applies unchanged — separate, unmodified component, credit
  in `licenses-*.txt`, APR precedent. Two additions: the repository ships a NOTICE file whose
  attribution text must be reproduced (Apache 2.0 §4(d)), and "Basis Universal" is a trademark, so
  the credit names it without implying endorsement.

### Licensing

Recorded because it constrains the design, not as an afterthought. **Not legal advice — flagged for
the owner.**

- Soapstorm inherits **LGPL v2.1** (`doc/LICENSE-source.txt`, "version 2.1", with no "or later").
- Basis Universal (bc7f) is **Apache 2.0**, from Binomial LLC. So was bc7e before it, so the analysis
  below is unchanged by the 2026-09-09 swap; what changed is listed at the end.
- The FSF treats Apache 2.0 as **incompatible with GPLv2/LGPLv2.1**: its patent-termination and
  notice terms are further restrictions those licences do not permit. It *is* compatible with
  (L)GPLv3, which is not an option here — there is no "or later".
- But this tree **already ships Apache-2.0 third-party code**: APR is Apache 2.0 and is listed in
  `indra/newview/licenses-*.txt`. The project's established posture is that an Apache-2.0 component
  is acceptable as a *separate library* with its notices preserved, the same treatment cURL, expat
  and OpenSSL get. That is the precedent the Basis Universal transcoder is placed under, deliberately
  and not by accident.

What that means concretely, and why the code looks the way it does:

1. The transcoder lives **unmodified** in `indra/llimage/basis_universal/transcoder/` with the
   upstream `LICENSE` and `NOTICE` beside it and a README recording the upstream commit. Not one
   line of it is copied into any file carrying our licence header, and no file in it is edited —
   editing would trigger Apache 2.0 §4(b) and break this posture. The zstd and encoder third-party
   parts of the upstream repository are not vendored at all.
2. `ssbc7block_bc7f.cpp` is **our** code under our licence. It calls bc7f only through the
   transcoder's public header. The transcoder compiles to its own object, at its own warning level,
   exactly like any linked library.
3. Notices are recorded in all three `indra/newview/licenses-*.txt`, which ship as the installer's
   `licenses.txt` — that is the copy that satisfies the licence. Because upstream ships a NOTICE
   file, §4(d) applies: its attribution notices are reproduced there verbatim (ASCII-folded).
4. The About box reads build-generated `packages-info.txt`, which is produced from the autobuild
   packages and therefore cannot know about a backend chosen at configure time. So the linked
   backend supplies its own credit via `ssBC7BlockBackendAttribution()`, returning null for the
   portable one. Asking the backend rather than testing a build flag means the credit shown can
   never describe code that is not in the binary.
5. "Basis Universal" is Binomial's trademark. The credit names it and claims nothing.

**The hedge is structural, and it is the reason to be relaxed about all of the above.** bc7f is a
configure option: `SS_BC7F=OFF` builds a viewer with none of the transcoder in it, using
`ssbc7block_mode6.cpp`, which is entirely ours. If the licence question is ever decided against us,
backing out is a configure flag and a measured quality regression on multi-colour blocks — not a
rewrite. That is worth more than any confidence about how the compatibility argument would land.

What the 2026-09-09 swap removed: bc7e (`indra/llimage/bc7e/`, `ssbc7block_bc7e.cpp`), the ISPC
toolchain hook (`indra/cmake/ISPC.cmake`, `SS_ISPC_EXECUTABLE`), and the BC7E and Intel ISPC
sections of `licenses-*.txt`. The ISPC install on the owner's machine and the `SS_ISPC_EXECUTABLE`
line in the gitignored `build.ps1` are no longer read by anything.

## The four critical fixes (designed in from day one — each was a verified would-have-shipped bug)

1. **Explicit-format lifecycle (guaranteed crash otherwise)**: setExplicitFormat(BPTC) leaves mHasExplicitFormat + BPTC in mFormatPrimary; the reset guard at llimagegl.cpp:1575-1582 does not cover BPTC. The first time a BC7 preview is upgraded by a progressive J2C fetch, setImage computes is_compressed from the stale format and hits the LL_ERRS at llimagegl.cpp:991. Every BC7→uncompressed re-create must clear/re-derive formats as a first-class state transition (also fires on readback→refetch and raw-consumer paths).
2. **Source components injection (alpha-pool misclassification otherwise)**: BPTC is intrinsically 4-channel; pipeline.cpp:1935 / llface.cpp:346 classify alpha-blended faces by getComponents()==4. Without a stored source-components byte injected at upload, every opaque RGB texture served from BC7 lands in the alpha pool — sorting cost, draw-order artifacts, large perf regression. OWNER FOLLOW-UP (other BC formats "if no quality loss"): this is a metadata bug, not a format problem — the stored components byte fixes it completely at zero quality cost. No alternative BC format helps: BC1 (4bpp, opaque) halves footprint but has real quality loss (gradient banding); BC4/BC5 are single/dual-channel (wrong for color); BC6H is HDR-only. BC7 is already the max-quality block format; BC1-for-opaque stays a later measured OPTION, never a correctness fix.
3. **VRAM accounting (the success metric would be fiction otherwise)**: alloc_tex_image is called only inside setManualImage (llimagegl.cpp:1491); the compressed branch (796-801) never accounts. BC7 textures would be invisible to getTextureBytesAllocated — the A/B would over-report and the discard-bias controller would mis-steer. Add explicit alloc/free_tex_image to the compressed path.
4. **Purge cascade (privacy lie otherwise)**: the rename-wipe covers only purgeAllTextures(true); purgeAllTextures(false) (mid-session clear) and clearCorruptedCache never touch bc7cache — "clear cache" would leave a complete BC7 copy of every texture plus manifests recording where you went and whom you saw. Hook ALL purge sites; close store handles before wipe (open handles also break the Windows dir-rename trick for the J2C cache).

Plus smaller specified fixes: plain-UUID encode dedupe (sufficient under full-res-only — records are immutable per (UUID, encoder_version), so there are no discard improvements to supersede); raw-needing callback acquired after BC7 residency forces the J2C path (callback starvation otherwise); pick mask computed at encode at PARITY resolution (base level, not a 128px cap — coarsening it changes picking behavior on vegetation/fences); blob-read failure → tombstone + J2C fallthrough; BC7 residents exempt from mDownScaleQueue (scaleDown's FBO path is illegal on BPTC); VRAM-pressure down-rez via RAM-resident tail mips (sub-128px mips are tiny) with blob re-read fallback — never a disk-read storm at the worst moment; flush encode queue on teleport (queued raws pin up to 256 MB).

## Read path (P2, as built)

Three stages on three threads, and the split is forced by what each API locks rather than chosen.

**PROBE (main thread, no IO)** — `ssBC7ServeProbe` in `LLViewerTextureList::createImage`, after the boost level is applied and BEFORE fast-cache enrolment. After, because half the exclusions key on boost; before, because a fast-cache hit is a 16x16 raw that goes straight to `addToCreateTexture` and would clobber a BC7 upload with a thumbnail — so a probe hit suppresses enrolment entirely. It uses `SSBC7Store::lookup` rather than `hasRecord`: one map find under one lock acquisition returns the whole record (dimensions, mip count, source components, flags), which is everything needed to size the texture before the blob arrives, and it stamps the heat signal so a read-served texture never ranks cold in eviction. A second, once-per-texture probe sits at the top of `updateFetch` as the safety net for everything `createImage` cannot reach: textures built before the store came up, textures created through `getImageFromUrl`, and everything that already existed when the user ticked the box mid-session.

**READ (reader pool, blocking IO)** — `SSBC7Store::readBlobPrefix(id, levels, ...)`, holding no lock across the IO. Serving desired discard d reads `mip_count - d` store-order levels, which is a contiguous PREFIX because the payload is written smallest-mip-first, verified against the per-level CRCs the writer records in store order. `readBlob` keeps its full-payload contract for callers that want the whole blob. The pool is a separately named `LL::ThreadPool("BC7Read")` at NORMAL priority, deliberately not the encode pool: the encode workers run under `THREAD_MODE_BACKGROUND_BEGIN`, which on Windows deprioritises disk IO as well as CPU — exactly wrong for a read that gates a visible texture — and their queue is shared with multi-hundred-millisecond encodes. `tryPost` only; a refusal is a counted verdict and never a block.

**UPLOAD (main thread)** — `ssBC7ServePumpUploads` from `LLViewerTextureList::updateImagesCreateTextures`, inside that function's existing per-frame budget rather than a second one. The blob is owned by the work item and moved through a mutex-guarded completion list; it is never parked on the `LLViewerFetchedTexture`, which is not thread safe and whose raw-image members are already contended. One reference is taken on the main thread before the post and released by the pump, mirroring `scheduleCreateTexture`.

**Residency ladder** lives on `LLViewerFetchedTexture` (`NONE → HIT_KNOWN → READING → RESIDENT(d)`, plus a terminal `DECLINED` carrying the reason). DECLINED is the fifth value the original design did not name and it is load bearing: without it an excluded texture is re-probed every frame, and the log cannot tell "never considered" from "considered and refused" — the exact failure the encode side already paid for. Every verdict is counted and the tally is written to `LL_INFOS("Squeeze")` on a 60 second tick whenever it has changed, with a live gauge of resident textures and video memory saved *right now* rather than a high-water mark.

**Suppressing the J2C fetch is the whole saving** and needs almost no new logic: once a BC7 upload sets `mCurrentDiscardLevel`, `updateFetch`'s existing `current_discard <= desired_discard` test stops requesting by itself. The one addition is that `make_request` is forced false while the ladder is at READING — otherwise a warm J2C cache frequently wins the race and the video memory the record exists to save is spent anyway. That suppression is bounded by construction: the reader pushes a completion on every path including abandon, and a refused post never enters READING at all.

**Upload contract** (asserted, not assumed, at the call site): `setSize(full_w, full_h, record.mSrcComponents, -1)` — full-resolution dimensions and a NON-POSITIVE discard, because `mMaxDiscardLevel = llmax(mMaxDiscardLevel, discard_level)` would otherwise inflate the level count past the stored chain on a small texture and the backward walk in `setImage` would read past the end of the prefix, silently. Then `mMipCount == mMaxDiscardLevel + 1` is checked outright, then `setExplicitFormat(GL_COMPRESSED_RGBA_BPTC_UNORM, GL_COMPRESSED_RGBA_BPTC_UNORM)` with internal and primary the SAME enum because `alloc_tex_image` sizes from primary while `setManualImage` sizes from internal. `mFullWidth`, `mFullHeight`, `mComponents` and `setTexelsPerImage()` are mirrored by hand afterwards, because there is no `LLGLTexture` wrapper for the `data_hasmips` overload and `processTextureStats` divides by `mTexelsPerImage`.

**Explicit-format lifecycle** is now a named transition, `LLImageGL::dropCompressedFormat(reason)`, which clears the flag AND re-derives the uncompressed format in the same breath — clearing alone leaves a window where `isCompressed()` still answers true, and that window is the crash. It is called deliberately from every BC7 exit (`addToCreateTexture`, `preCreateTexture`, `forceToSaveRawImage`, `forceToRefetchTexture`, `setLoadedCallback`, `clearFetchedResults`, `destroyTexture`) and logs at debug level, because in a healthy session that transition is entirely ordinary. The guards inside `setImage` and `createGLTexture` remain as genuine last resorts and warn, because arriving there means a caller failed to declare itself; the one inside `setImage` in particular converts what was a fatal `LL_ERRS` into one lost texture.

**Pick masks are stored (since 2026-09-09; before that they were the one deliberate limitation).** A BC7 upload has no alpha bytes for `LLImageGL::updatePickMask` to read, so without a mask `getMask` answers TRUE for the whole quad and every fence, plant and hair card stops being click-through - which is why alpha textures were declined (`SSBC7_SERVE_DECLINE_ALPHA`) and `SSSqueezeServeAlpha` existed to measure the cost. Now the encode worker builds the mask from the decoded base level (`buildPickMask` in `ssbc7encodequeue.cpp`, bit-identical to `updatePickMask`'s layout: one bit per 2x2 cell, alpha > 32, at PARITY resolution rather than a cap), the store keeps it after the payload with its own CRC, `readBlobPrefix` fetches it in the same open as the prefix, and `LLImageGL::ssSetPickMask` installs it after the compressed upload. An alpha texture is served whenever its record carries `SSBC7_FLAG_HAS_PICKMASK`; a record without one is still declined with the same counted reason. `SSSqueezeServeAlpha` is removed. The alpha SHAPE is already carried (`SSBC7_FLAG_ALPHA_IS_MASK`) and is injected through a tagged `LLImageGL::setIsAlphaMask`, because `calcAlphaChannelOffsetAndStride` forces `mIsMask` false for BPTC and `analyzeAlpha` never runs.

**Down-rez** re-uploads a shorter prefix instead of queueing. `LLImageGL::scaleDown` refuses a block compressed texture and `processTextureStats` calls `scaleDown()` every pass while current is below desired, so a BC7 resident left on `mDownScaleQueue` is re-queued forever, `mCurrentDiscardLevel` never moves and the memory governor never gets those bytes back — the opposite of the point, at the moment it matters most.

## Promotion engine (P4, as built)

IMPLEMENTED 2026-08-27, in `indra/newview/ssbc7promote.cpp` with the decisions split into `ssbc7promotepolicy.cpp`. This closes the gap the read path exposed: `ssBC7EncodeWantList()` had zero callers anywhere in `indra/`, `SSSqueezeNetworkPromote` had a preferences checkbox and no reader at all, and the encode pool logged "in flight 0" in twenty of twenty-seven samples while the want list only grew. Under full-res-only that is not a missing optimisation, it is the fill mechanism being absent.

TWO TIERS IN STRICT ORDER, and the order is the whole safety argument. Tier (b) is not considered at all until the want list is empty.

**TIER (a), free, no network.** When `ssBC7EncodePendingCount()` is zero - the demand path has nothing in flight - a bounded SCAN is posted onto the existing BC7Encode pool. It takes the newest want-list uuids (48, or four per spare worker if that is more), drops those the store already has, and asks `LLTextureCache::ssProbeJ2C` whether each J2C is complete on this disk. **CAPACITY-DRIVEN since 2026-09-09.** There is no fixed number of fused encodes per pass. The tick measures SPARE WORKERS - pool width, minus workers inside an encode right now, minus fused encodes already posted that no worker has reached - and offers exactly that many, gated on the adaptive controller: HEADROOM ("keeping up easily") gets the full count, HOLDING gets a trickle of one so the controller keeps receiving samples, STEPPED_DOWN gets nothing. Other load on the machine shows up in the controller as falling throughput, so this is the system-stress signal as well as the texture-volume one. While anything is waiting the pass cadence is `SSBC7_PROMOTE_FOLLOWUP_SECONDS` (0.25 s) rather than the 2 s idle tick. The previous fixed ceiling of four per two seconds was sized for bc7e and left sixteen cores idle behind a list of two hundred textures - the owner's rule that replaced it: "if there is a spare core and it is keeping up easily, add another to the pile". COMPLETE ones are claimed and posted as fused items: `[read -> decode -> mips -> encode -> store]`, one work item, wholly on the pool, never touching the ImageDecode pool. PARTIAL ones move to the network candidate list. NO_ENTRY, UNKNOWN and READ_FAILED are dropped with their own counted reason, because keeping them would make the want list a list of textures the engine can never act on.

COMPLETENESS is decided by the J2C tier's own partial sentinel and nowhere else: `LLTextureCacheWorker` stores the asset's total size in the entry, but a fetch that only had part of it stores total+1 (`lltexturefetch.cpp:2079-2088`), so "header record + body >= total" makes a partial fall exactly one byte short. That arithmetic is `ssBC7J2CExtent` and is unit tested offline against both the real record size and a different one, because the rule must not depend on `FIRST_PACKET_SIZE`.

**TIER (b), network, and deliberately hard to notice.** Governed by `SSSqueezeNetworkPromote`, which now actually reads. Refused unless: the want list is empty, the store is under its budget, the encode pool is idle, the session is at `STATE_STARTED` and connected, no teleport is in progress, the texture fetcher has at most four requests outstanding for the USER (llviewertexturelist.cpp:924 uses zero for the same question; the allowance stops one lingering DONE worker from disabling the engine for a session), at most two continuation fetches are already in flight, a token bucket at `SSSqueezeNetworkPromoteKBPerSec` holds enough credit for the missing bytes, and the whole-session ceiling `SSSqueezeNetworkPromoteMaxMB` has not been reached.

The rate limit and the ceiling are both present because they answer different questions: the bucket says "never faster than this", the ceiling says "never more than this, at all". A rate limit alone still spends a gigabyte given a long enough session, which is exactly what somebody on a metered connection is afraid of.

Both are read in `ssBC7PromoteRefreshPolicy` and pushed into `SSBC7NetBudget::configure`, so a changed setting takes effect on the next pass; re-reading is safe because `configure` only sets the two limits and trims held credit down to the new rate, never the session total, so nobody earns a fresh allowance by opening preferences. `SSSqueezeNetworkPromoteKBPerSec` 0 switches the whole tier off (an empty bucket refuses everything) and `SSSqueezeNetworkPromoteMaxMB` 0 means no ceiling. A refusal by the budget is a DEFERRAL: the candidate goes straight back on the list, the pass stops issuing, and it records `NET_BANDWIDTH` or `NET_SESSION` - so a throttled engine reads as throttled in the overlay instead of as `NET_NO_TARGET`, which is what it looked like while these two keys were missing from `settings.xml` and the budget sat at its constructed rate of zero (fixed 2026-09-10; until then tier (b) could never issue anything at all).

TARGETING, with manifests still unbuilt: a candidate must still have a live `LLViewerFetchedTexture` in `gTextureList`, be `FTT_DEFAULT`, want no aux channel, have no record already, pass the encode geometry rule when its dimensions are known, and NOT currently be fetching for the user. A texture the viewer has already forgotten never gets bandwidth. This is the "manifest-referenced or recent last_use" rule expressed with what exists today; when P5 lands, the manifest becomes the better signal and slots in at the same point.

THE REQUEST IS THE PIPELINE'S NATIVE MODE. `createRequest` at discard 0 asks for the whole asset and the worker resumes from what is already cached using the same offset arithmetic every progressive fetch uses. The completed J2C is written back by the ordinary `WRITE_TO_CACHE` state and the encode is triggered by the ordinary `DONE` hook, so this engine adds no transfer code, no cache code and no encode code - only the decision to ask. Priority is `1.f`, not zero, because `lltexturefetch.cpp:1158` aborts a worker below `F_ALMOST_ZERO`; one is the lowest value that still means "eventually" and sits below every priority the texture list assigns. Completion is detected with `getRequestFinished`, the same call `updateFetch` makes; a request is only deleted when the user's own texture has not taken the worker over, and both run on the main thread so that check is exact rather than merely likely. A teleport wipes every fetch, so the slots are released rather than waiting out their 120 second timeout.

**BOUNDED, AND IT SAYS WHAT IT DROPPED.** The want list was a plain set that simply refused new entries past 8192 - which pins it to whatever the first eight thousand textures of the session happened to be and then reads from a log line exactly as though the engine had everything in hand. It is now `SSBC7WantList`: FIFO at the bottom, LIFO at the top. Work is taken NEWEST first because the most recently seen texture is the one the user is probably still stood in front of; entries are dropped OLDEST first so a walk through a mall cannot push out the room they are actually in. Every drop is counted, the first and every thousandth is logged with the uuid, and the drop total is exposed so a readout can say that "still waiting" is a FLOOR rather than a total. The 256-entry network candidate list behaves the same way.

**THE RE-ARM, which the read path needed and did not have.** `ssBC7ServeProbe` latches `DECLINED` with `SSBC7_SERVE_DECLINE_NO_RECORD` and never looks again, so a record created mid-session was not served until the next login - throwing away the video memory the engine had just spent CPU and bandwidth to make available. Every uuid that reaches the store is now queued to the main thread, which resets the ladder to `NONE` for exactly those textures still sat at DECLINED-for-no-record; the safety-net probe in `updateFetch` re-examines them, so no exclusion logic is duplicated.

**SEAM, NOT A SECOND ENGINE.** Promotion borrows the demand path's pool, bounded queue, 256 MB pinned-raw budget and plain-uuid in-flight set through a small set of accessors in `ssbc7encodequeue.h`. Two pools would let a backfill encode sit ahead of a texture the user is looking at; two budgets would make the one 256 MB ceiling really 512 MB. Every post is `tryPost` and a refusal is a counted verdict that re-wants the texture, never a block - the frame thread posts these, and a blocked frame thread is a frozen viewer for work whose entire justification is that nobody is waiting on it.

LOCK ORDERING IS UNDISTURBED. `mHeaderMutex` before the BC7 store's locks already exists in the tree (`clearCorruptedCache` holds the header mutex through `purgeAllTextures` into `SSBC7Store::purgeAll`). This engine never nests the two in either direction: `ssProbeJ2C` takes the header mutex for one fixed-size entry read, releases it, and only then decodes and appends. The multi-megabyte body read holds nothing, which matters because the caller is a background-QoS worker whose disk IO Windows deprioritises - holding the header mutex across that would let a deprioritised thread stall the texture cache's own thread. `LLTextureCache::readBody` gained a `pool` parameter for the same reason `bodySize` always had one: its loose fallback used the cache THREAD's `LLVolatileAPRPool`, which a foreign thread must not share.

EVERY EXIT IS NAMED (`ESSBC7PromoteVerdict`): RAN_LOCAL, RAN_SCAN, RAN_NETWORK, IDLE_EMPTY, OFF, NOT_READY, POOL_BUSY, SCAN_RUNNING, POST_FAILED, BACKPRESSURE, STORE_FULL, NET_OFF, NET_TELEPORT, NET_LOGIN, NET_BUSY, NET_BANDWIDTH, NET_SESSION, NET_IN_FLIGHT, NET_NO_TARGET. There is deliberately no verdict for "the tick interval has not elapsed": that exit happens on almost every frame and changes nothing, so counting it would bury eighteen meaningful numbers under one eight-figure one. `ssBC7PromoteStatsNow()` exposes promoted-from-local, completed-over-network, still-waiting, want-drops, candidates, in-flight, bytes spent against the ceiling and the last verdict, as numbers, for the overlay.

DEFAULTS: `SSSqueezePromote` on (tier (a) costs no download at all), `SSSqueezeNetworkPromote` on per OWNER CALL #4, `SSSqueezeNetworkPromoteKBPerSec` 128, `SSSqueezeNetworkPromoteMaxMB` 512. The last two have no preferences UI yet; the existing checkbox is the one that stopped lying.

## Adaptive encode quality (as built)

IMPLEMENTED 2026-08-28, in `indra/newview/ssbc7adaptive.cpp` with the decisions split into
`ssbc7adaptivepolicy.cpp` so the offline harness runs the shipping control law rather than a copy.
Measure what the pool is really achieving, pick the profile that matches the backlog, and — the half
that matters more — go back over what was rushed once the machine is idle.

### The record format change, which had to come first

Quality used to be folded into `ssBC7BlockBackendVersion()` (100 + quality), which is stamped into
every blob and compared at startup: a mismatch WIPES the tier. That is fine when the profile is
chosen once per session. It is catastrophic when a controller changes it every few minutes, so the
scheme had to go before anything adaptive could exist.

**The profile now lives in the record.** `SSBC7Record` gains a `mQuality` byte; the fields it
already serialised totalled 50 bytes plus a 4 byte checksum against `SSBC7_RECORD_SIZE` of 64, so
ten were spare and this spends one. The blob header carries the same byte in one of its reserved
two, because the header's stated purpose is that the segments alone can rebuild a lost index — and a
rebuild that forgot the profile would send the upgrade pass over everything. `SSBC7_FORMAT_VERSION`
moves 1 → 2 for this, once, and the intention is that it never moves for quality again.
`ssBC7EncoderVersion()` also changes (0x00010066 → 0x00010064 on bc7e) because the quality term left
it, so existing caches are wiped exactly once on the build that lands this.

A quality byte outside the known range is **clamped on read, never rejected**. `loadIndex` stops at
the first record that fails, so refusing one over a byte that only steers a background pass would
silently discard every record written after it.

This is not damage control, it is what makes the feature good: with quality per record the store may
hold a mixture, and a record encoded in a hurry becomes something to IMPROVE rather than something
that is wrong until the next wipe.

### Lock-free profile selection

`ssBC7EncodeBlocksRGBA` takes the profile as an argument. bc7e's parameter block for **every** rung
is built once inside the existing `std::call_once` and is read-only thereafter, so selecting between
them is an array index — no mutex, no atomic, nothing to synchronise on a path that runs once per row
of blocks. `ispc::bc7e_compress_blocks` takes a `const` params pointer, which is what makes that
sound. Re-initialising one shared struct on change would instead have put a writer on the hot path of
sixteen readers to save three hundred bytes.

The encode path reads the current profile once per texture, on the worker, at the moment the encode
starts — not when the item was queued. A queue that has been sitting for a minute would otherwise
write at a profile the controller has since abandoned, and its own depth is the evidence that the
older answer was wrong.

### What is measured, and over what window

Per profile, on the worker side, bracketing `ssBC7EncodeMipChain` and **nothing else** — not the J2C
read, not the decode, not the alpha classification, not the store append. Folding in work that is
identical at every profile would flatten the ladder and make a step down look as though it bought
almost nothing.

- Numerator: `SSBC7EncodeResult::mEncodedTexels`, the padded block grid across the whole chain, which
  is the work the backend really did. Deriving it from the base size would flatter small textures.
- Denominator: that one worker's wall time. So the figure is **per worker**, and pool capacity is it
  multiplied by the pool's real width (`ssBC7EncodePoolWidth()`, not the setting, which is zero on
  every machine that leaves it automatic).
- Window: a 64-sample ring per profile over the last 60 seconds. If fewer than four samples fall
  inside the window the whole ring is used regardless of age — the pool is supply limited, so a
  healthy machine genuinely goes a minute without encoding, and a stale measurement of THIS machine
  beats a benchmark figure from a different one. Benchmark seeds are used only for a rung nothing has
  been encoded at yet.
- Samples with zero duration or zero pixels are dropped, not clamped: an infinite rate would justify
  the best profile forever.

### The control law

Backlog is the want list plus demand encodes in flight, in textures; the window's mean megapixels
per texture converts it to work. The single quantity the law turns on is **estimated seconds to
drain**, `backlog x mpix_per_texture / capacity`, and an unusable capacity returns −1 for "unknown",
which every caller treats as hold rather than as instant.

- **Step down** when the CURRENT profile's drain estimate exceeds 300 s.
- **Step up** when the profile ONE RUNG BETTER is predicted under 90 s.

Testing the step up against the TARGET rather than against the current rate is what makes it stable
rather than merely slow. Asking "is here comfortable" would climb whenever a cheap profile had caught
up, onto a profile eight times slower that then immediately falls behind — an oscillation with a
period of a couple of minutes, scattering mixed-quality records for no benefit. Asking "would there
be comfortable" applies both tests to the same quantity with a dead band between them, so a step up
cannot undo itself. This is pinned by a test.

Hysteresis, all four of which are required and none of which is sufficient alone: the 90/300 s dead
band; a 20 s dwell down against a 90 s dwell up (falling behind costs the user time, catching up too
early costs records the upgrade pass must redo); a 60 s cooldown between any two changes, armed from
startup; and both dwell timers cleared on every change, so one observation can never produce two
moves. Either dwell timer is reset the instant its condition stops holding, so a noisy signal never
accumulates — the offline test flaps the backlog every tick for thirteen minutes and the ladder does
not move once.

**The user's setting always wins.** `SSSqueezeEncodeQuality` gains value **3, adaptive, as the new
default**; 0–2 still pin a profile exactly as before. It is an added enum value rather than a second
checkbox so "which profile am I getting" stays one question with one answer, and so a user who pinned
HIGH in an earlier build keeps HIGH. The pin is checked before anything else is computed, so there is
no path by which a measurement overrides it. The offline test drives every pinned value past a
hopeless backlog and a completely idle machine for over an hour each and asserts it never moves.

Every transition is logged at `LL_INFOS("Squeeze")` with the profile either side, the reason, the
backlog, the measured capacity, the worker count, the drain estimate and both bars. Reaching the
bottom rung additionally logs a warning, because ultrafast shares the portable backend's 11 dB
blind spot on multi-hue blocks and the only reason it is on the ladder at all is that the upgrade
pass will come back for it.

### The idle upgrade pass — tier (c) of the promotion engine

This is the real answer to "if it is a slow day use the best profile": not only encode new work
better, but redo what was rushed. It is a third tier of the EXISTING promotion engine rather than a
second scheduler — same pool, same claim, same pinned-bytes budget, same fused
`[read → decode → mips → encode → store]` item, same idle detection.

It runs only when tiers (a) and (b) have nothing left, and never when the network gate said the user
is busy (NET_BUSY, NET_TELEPORT, NET_LOGIN): a free encode pool is not the same as a calm machine,
and a teleport arrival has one for the second before the textures land. It is also refused whenever
the controller has stepped down — a machine behind on new work has no business redoing old work —
which is the second half of the control law rather than a separate policy.

`SSBC7Store::takeUpgradeCandidates` walks the index on a pool worker (never the frame thread) and
returns records below the target profile **worst first**, since a FAST record is visibly wrong in a
way a BALANCED one is not. `append` gained an `allow_supersede` argument, off for every ordinary
append: superseding writes a newer record for the same uuid, which `loadIndex` already resolves in
favour of the later one, and the old blob becomes garbage its segment reclaims when killed. A
supersede that is not STRICTLY an improvement is refused and counted — otherwise a mistimed pass
rewrites the store to change nothing.

A candidate the pass cannot finish (almost always a J2C that has since left the texture cache) is
written off into a capped skip set and counted, rather than being re-read, re-decoded and re-refused
every pass for the rest of the session. It still counts toward "how many are left", so the readout
cannot claim the pass finished when it merely gave up.

Gated by `SSSqueezeUpgradeIdle`, default on. New verdict `SSBC7_PROMOTE_RAN_UPGRADE`, appended to the
enum rather than inserted, because the ordinals index the verdict-name table.

### The readout

`ssBC7AdaptiveStatsNow()` returns `SSBC7AdaptiveStats` as plain numbers, in the style of
`SSBC7PromoteStats`, for the owner to wire into the overlay separately — nothing here edits
`ssstatsview.cpp`. It answers four questions:

1. **Which profile, and why.** The profile plus an `ESSBC7AdaptReason`: pinned by the user, no data
   yet, best quality keeping up easily, holding, stepped down under backlog, stepped back up as it
   cleared, or this backend has one encoder. A transition reason is STICKY until the next transition
   or until the ladder is back at its best rung with headroom, because an overlay reads this between
   ticks and a reason overwritten on the next tick could never be seen. Plus the session change count.
2. **How the cores are used.** Pool width, a ~10 s moving average of workers inside an encode (an
   instant sample on a supply-limited pool reads zero most of the time and would look broken), and
   cumulative worker busy seconds for an overlay that would rather difference it itself.
3. **The rate.** One aggregate figure — megapixels in the window over the window's own wall span,
   which idle workers correctly pull down — for display, plus the per-worker per-profile table and
   its sample counts for detail, plus the capacity the last decision was actually made against, the
   backlog and the drain estimate.
4. **The upgrade pass.** Records improved this session, records still below the best profile, whether
   a pass is running right now (a nesting COUNT, not a flag, so it stays true while the re-encodes it
   posted are running rather than only while the scan is), how many were written off, and the full
   per-quality histogram. The histogram is maintained incrementally by the store on every append,
   supersede, kill, compaction and index load, so reading it costs a counter copy and never a scan.

`ssBC7AdaptiveMetricsString()` remains for the log and is written on the promotion engine's existing
120 s tick, so a quiet session still leaves one honest statement next to the backlog it was choosing
against.

### Verified

Offline: `test_bc7adaptive.cpp` drives the real `ssbc7adaptivepolicy.cpp` — window arithmetic and its
refusals, step down, step down twice, step up, the step-up-is-not-self-defeating case, two shapes of
noise, every pinned value, and the no-measurement case. `test_bc7store.cpp` gained the quality byte
round trip, the out-of-range clamp, the blob header, supersede accept/refuse, the histogram, candidate
selection worst-first with a skip set, and survival across a reopen; its existing 400-odd assertions
including the whole eviction and crash-recovery suite still pass on the new 55-byte record.
`test_bc7enc.cpp` gained a check that the profile REACHES the backend — the same noisy image encoded
at two profiles must differ on bc7e and must match on mode 6 — and that per-call selection leaks no
state between calls; it passes against both backends. `/Zs` semantic parse against the real viewer
headers is clean for every touched file, including all three `indra/llimage` sources.

## Region manifests (P5, as built)

IMPLEMENTED 2026-08-28, in `indra/newview/ssbc7manifest.cpp` with the file format and the cap policy split into `ssbc7manifestfile.cpp`. This closes the gap the read path leaves open: the store only ever helps once something ASKS for a uuid, so on arriving somewhere you have been before, every texture still waits for the interest list to reach it, the fetcher to request it and the pipeline to decide it is wanted - while the bytes sit on the local disk in the exact form the GPU wants them.

SCOPE: region manifests only. Agent manifests are the same mechanism keyed by avatar uuid and hooked at `processAvatarAppearance` instead of region change; the file format, the cap policy and the pre-warm are all key-agnostic, so adding them later is a second filename prefix and a second recorder rather than a second design.

**WHERE IT LIVES, and why not in the .roc file.** `<cache>/texturecache/bc7cache/manifests/r_<grid>_<x>_<y>.uml`. `ssroccache.h` does define a `SSROC_SECTION_MANIFEST` and folding this in there is a sensible consolidation ONCE ROC HAS SETTLED - but a texture manifest is a Squeeze concern that merely happens to be keyed by region, and putting it in ROC's file makes every ROC format change a Squeeze format change while two workstreams are live in `ssroccache.cpp`. Living under `bc7cache` also buys the privacy requirement for free: `SSBC7Store::purgeAll` is a recursive `deleteDirAndContents` of the store directory, so "clear cache" takes the record of where you went with it, and that falls out of critical fix 4 rather than needing a second hook to forget. Grid-scoped names because SL and OpenSim coordinates collide outright.

FORMAT: a 64 byte header (magic, version, region handle, last_seen, count, lifetime drop total, visit count, payload CRC, header CRC over the first 60 bytes) plus a flat 20 byte stride of uuid + weight. Rewritten whole through a temporary and a rename, so the torn file the reader refuses is one the writer cannot produce. **Manifests are DERIVED DATA and a version bump orphans them**, which is the established precedent - the store itself wipes on encoder-version mismatch, and the whole cost is one region visit spent re-learning. Stated rather than smuggled into a silent reformat.

EVERY REFUSAL IS NAMED (`ESSBC7ManifestVerdict`): OK, NO_FILE, UNREADABLE, SHORT, MAGIC, VERSION, HEADER_CRC, COUNT, TRUNCATED, PAYLOAD_CRC, HANDLE, WRITE. Nothing below the checksum is believed before it passes, and **the count is why**: a flipped bit in the count field is the one corruption that turns a bad file into a bad allocation, so the checksum is tested before the count is read. The size check is an equality rather than an "at least", because a file LONGER than its header claims - what overwriting a big manifest in place with a smaller one leaves - is exactly as untrustworthy as a short one. The header carries the region handle and it is checked against the filename, for the same reason `ssBC7VerifyBlob` requires a uuid: a filename is precisely the part of a cache that survives being copied, renamed or restored from somebody else's backup.

**HOW THE REGION CHANGE IS DETECTED: no new stock hook at all.** `ssBC7ManifestTick()` is called from the head of `ssBC7PromoteTick()`, above every gate there so manifests are not switched off by `SSSqueezePromote`. That tick already runs on the main thread every frame and `gAgent` already knows which region the agent is in, so the whole detector is one pointer dereference and one 64 bit comparison. Nothing was added to `llviewerregion.cpp`. A null region is ignored rather than treated as a departure, because a teleport passes through one.

**RECORDING** is one walk of `gTextureList` every 15 seconds on the main thread - the same shape as the eviction heat sweep, at a quarter of its rate - and never on a fetch thread. The signal is `mMaxVirtualSize`, which the texture list has already computed: a non-zero one means something on screen wanted this texture, which is the definition of "mattered here". The set is not restricted to objects owned by the current region on purpose: a neighbour's build is on this screen, so the manifest describes a VIEWPOINT rather than a parcel.

**WHAT THE CAP DROPS.** 4096 uuids per region, 128 regions. The want list is a work queue so its oldest entry is its least urgent and FIFO is right there; a manifest is a description of a place, read back once in full, so age says nothing and area says everything. The cap therefore sacrifices the LIGHTEST entries - those only ever glimpsed a few pixels across, simultaneously the cheapest to forget and the least missed - and the pruning is hysteretic (overshoot by an eighth, cut back to the cap) so a texture-heavy region does not pay a sort per insertion. Every drop is counted, the last one is kept so a log line can name it, and the lifetime total is written into the manifest header so a readout says "at least this many" instead of implying it remembered everything. A merged-in manifest has its weights HALVED, so a set built over many visits keeps re-earning its place rather than ossifying around the first visit.

**WHAT BOUNDS THE PRE-WARM.** Five independent limits, and they stop different things: at most 32 reads posted per 0.25 s pass (the shared reader queue is 256 deep, so this path can never fill it and leave a foreground read with nowhere to go); at most 128 entries EXAMINED per pass, which is what bounds the main thread on a cold store where every entry costs a lookup and returns no-record; at most 2048 attempts per arrival; a video memory budget, `SSSqueezeManifestWarmMB`, default 128 MB; and the pass refuses outright while `ssBC7ServeReadsInFlight()` is non-zero, which is how "foreground fetch always wins" becomes a gate rather than a hope. When the budget runs out the pass STOPS rather than skipping to something smaller, because the queue is in descending weight order and everything past that point matters less than what is already placed.

**THE WARM IS AT DISCARD 2, NOT FULL RES**, and this is the number the whole video memory argument rests on. A full-res warm of a dense region would be a gigabyte spent on textures nobody has looked at; discard 2 costs about a sixteenth of that, is already sharper than anything the network could have delivered in the same instant, and the ordinary `RESIDENT -> READING` edge in `updateFetch` sharpens each texture to its real wanted level the moment an object references it. Warming cheap and upgrading on demand beats warming expensively and evicting.

**CREATING THE TEXTURE OBJECT IS SAFE, and the reason is load bearing.** The pre-warm calls `LLViewerTextureManager::getFetchedTexture(id, FTT_DEFAULT, ..., LOD_TEXTURE)` for uuids nothing has asked for yet. `decode_priority` in `updateFetch` is `mMaxVirtualSize`, which is zero until something on screen wants the texture, and `make_request` is false at zero - so a pre-warmed texture can never turn into network traffic. If nothing ever references it, the ordinary lazy flush in `updateImageDecodePriority` deletes it after thirty seconds of no references and hands the video memory straight back. The whole episode is self-cleaning, which is why the budget is a ceiling on a temporary cost rather than a permanent one.

**ONLY UUIDS THAT ACTUALLY HAVE A RECORD.** A manifest entry the store has no record for is counted (`mWarmNoRecord`) and dropped. It is never turned into a network fetch from this path - that is the difference between a warm-up and a speculative download, and it is what lets this path promise it reads the local disk and nothing else. Feeding those uuids to the promotion want-list is the obvious next step and is deliberately not taken here.

**METRICS**, exposed as numbers through `ssBC7ManifestStatsNow()` for the overlay the owner wires separately: manifests loaded, refused and saved; uuids named; pre-warms issued and, separately, LANDED - confirmed resident by a bounded sweep, because a posted read is not a resident texture; already resident; no record; declined; reader busy; the BC7 bytes and the saving actually in place; and the recorder's depth and lifetime drop count. `ssBC7ManifestMetricsString()` writes the same on a two minute tick.

WHERE IT RUNS: the tick, the recorder and the pre-warm are main thread, because `gAgent`, `gTextureList` and the read path's request stage all are. The load and the save are the only file IO and both go on the existing BC7Encode pool via `ssBC7EncodeTryPost` - not a new pool, and not the reader pool, because an 80 KB manifest write has no business queueing ahead of a visible texture. The save deliberately does NOT honour the encode pool's abandon flag: shutdown posts it before the pool is told to abandon, and honouring the flag would lose the last region of every session to a race with whichever worker picked the item up.

STILL OPEN: the ROC miner. `doc/region_object_cache.md` describes ROC walking promoted objects' DP blobs at region-exit save to regenerate the manifest, which would populate a manifest for a region the user only walked through rather than stood in, and would pick up textures never rendered. That is a one-way ROC-to-manifest feed at a single call site and it slots in at `SSBC7ManifestSet::note` unchanged. Agent manifests, the location-TP-request warm-up (which can start during the teleport round trip rather than after arrival) and semantic eviction reading manifest `last_seen` as an evictability pin are all still outstanding.

## Work queue

Pool: LL::ThreadPool("BC7Encode") — the name auto-gains the ThreadPoolSizes override. Workers self-set THREAD_MODE_BACKGROUND_BEGIN inside an overridden run() — note the in-tree "precedent" at llappviewerwin32.cpp:1404 is itself broken (wrong-thread handle; the mode is valid only for the current thread), so this is the first working instance. Background QoS deprioritizes CPU, I/O, and memory pressure — priority via QoS, not width rationing. Conservative default width until throughput is measured (sleepy_robin idle workers Sleep(1)-poll; a wide idle pool burns wakeups).

Backpressure: bounded queue + tryPost only (WorkQueue::post blocks when full — a blocked fetch thread stalls every texture); ~256 MB cap on pinned raw bytes; overflow → drop to a want-list (lossless — J2C is on disk; Phase 4 backfill re-materializes). Pause during teleport/login decode bursts.

Promotion engine (Phase 4, CORE): fused [read complete J2C → decode → mips → encode → store] as ONE item wholly on the BC7 pool — the decode there has no latency consumer, and this keeps it out of the latency-critical ImageDecode pool entirely. Under full-res-only this engine is the PRIMARY fill mechanism, not an optional optimization: the demand path only encodes the free mHaveAllData-at-discard-0 case, so nearly all BC7 records are created here (preference order and network-completion throttles in the Encode pipeline section).

## Q1: why manifests beat physical region/agent data files

The dedup math, honestly: asset identity is a global UUID and cross-region/cross-agent reuse is the dominant cache economics. A handful of mesh bodies/heads/hair dominate what agents wear — 100 club-goers in 5 dominant bodies means per-agent files store the same multi-MB body textures ~20x. Full-perm kits and popular textures recur across most regions — a texture seen in K regions costs K× disk and K× write amplification per revisit. This compounds with BC7 already being 2-4x the J2C bytes. Once a global index exists (conceded), physical (c) has degenerated into (a)/(b) plus grouping metadata — paying duplication for nothing the metadata doesn't deliver. And mmap-a-region-file warming is a fiction after your first visit anywhere else: a revisited region is mostly already-cached-from-elsewhere UUIDs, never contiguous in a region file anyway.

Manifests over the shared store (Phase 5): tiny files `manifests/r_<grid>_<x>_<y>.uml` / `a_<agentuuid>.uml` (grid-scoped names — SL/OpenSim coordinates collide), header + UUID list + last_seen, rewritten whole, LRU-capped (~128 regions / ~512 agents, generalizing the VOCache mTime LRU). The Region Object Cache (doc/region_object_cache.md) is the primary region-manifest MINER: at region-exit save it walks promoted objects' DP blobs (TextureEntry + sculpt/mesh UUIDs) plus terrain detail UUIDs and EEP asset refs and regenerates the manifest — one-way feed, ROC→manifest, coupled at a single call site.

- TELEPORT WARM-UP: location TPs know the destination handle client-side at request time (llagent.cpp:5134-5150) — load the manifest, resolve against the RAM index, sort hits by (segment, offset), issue sequential prefix reads during the TP's network round-trip; landmark/lure TPs warm at RegionHandshake/loadObjectCache, where the already-loaded VOCache DP blobs are mined for texture/mesh UUIDs (the parser exists as partial-parse precedent, llviewerregion.cpp:2819). Warm-up lives in the store layer because gTeleportDisplay wipes all in-flight fetches (llviewertexturelist.cpp:841-855).
- AGENT WARM-UP: at processAvatarAppearance (llvoavatar.cpp:11114), load a_<uuid> and prefix-warm attachment assets (the stable cross-sighting payload; bakes churn per outfit). AttachmentBlock item-ID set = outfit fingerprint to skip warming unchanged re-sightings.
- SEMANTIC EVICTION: a record is evictable iff no manifest with last_seen < N days references it; deleting a manifest evicts a region/agent in one file operation. This gets shared assets RIGHT: an asset referenced by two regions stays until both age out — deleting region X's physical data file would either delete assets region Y still needs, or reveal there was never dedup.
- COVERAGE CAVEAT: TextureEntry mining misses GLTF/PBR material textures (base color/normal/ORM/emissive live in material assets + the GLTF extras cache) — exactly the content where VRAM pressure is worst; the miner must include the extras path.
- PRIVACY: manifests are an on-disk record of where you went and whom you saw wearing what. They MUST die on cache clear (falls out of fix 4). Posture beyond that is OWNER CALL #1.

## Q2: texture manager — deconstruction and verdict

What it actually is: LLViewerTextureList (vsize scan → log4 desired discard, round-robin update budget, discard-bias VRAM machine), LLTextureFetch (16-state worker machine + all cache traffic; transport encodes years of scar tissue — Varnish 1-byte range overlap, +1 sentinel, 404→UDP rollback, bake-URL retention), LLViewerFetchedTexture (per-texture glue, saved-raw entanglement, loaded-callback contract: multi-fire with improving discards, ALWAYS fires final on cleanup — bakes/bump/sculpt/UI depend on this), LLViewerTextureManager (stateless facade). External surface: ~160-234 call sites across 54-71 files, but wide-and-shallow (~25 methods + statics) — internals ARE replaceable behind the headers later.

Real defects (performance debt, not blockers): FIFO-not-priority fetch queue, poll-based completion (latency scales with texture count), coarse LRU, forceToSaveRawImage→refetch entanglement, mFullyLoaded never correct, TEX_LIST_SCALE dual-fetch race. None fights a sidecar; BC7 hits bypass most of the fetch pipeline, shrinking the population those defects apply to.

VERDICT: no rewrite now — 30-60+ dev-days of pure risk with zero VRAM benefit. Adopt now: (1) ITextureStore-style narrow interface in front of the new store so a later internal swap needs no consumer churn; (2) explicit BC7 residency ladder on LLViewerFetchedTexture (NONE → HIT_KNOWN → READING → RESIDENT(d), plus the RESIDENT→raw-needed and RESIDENT→uncompressed-upgrade edges — verification showed those two edges are where the crash and callback-starvation bugs live). REWRITE TRIGGERS (criteria, not schedule): (a) BC7 becomes the primary render path and J2C becomes cold storage, or (b) Phase 2-3 telemetry proves FIFO/poll latency dominates rez on warm caches. If fired: staged rewrite behind the preserved facade, keep the transport, legacy path selectable one release.

## Phases (hybrid plan)

- P0 (4-5d): GL groundwork — BPTC enums in the four llimagegl switches; explicit alloc/free_tex_image in the compressed branch; mHasBPTC + RGBA8 fallback; vendor bc7enc_rdo; debug command uploading one encoded texture via data_hasmips; decide RenderCompressTextures policy (BC7 supersedes driver compression where eligible; A/B controls for the setting — users already on it see ~1x VRAM delta, the pitch is quality/determinism/cached mips). EXIT: measured encode Mpix/s on target hardware.
- P1 (8-10d): write-only sidecar store — index+segments with CRC/truncate recovery; corrected record layout; two-lock discipline; plain-UUID dedupe; DONE-state enqueue (free full-res case) + want-list capture + exclusions; purge cascade at ALL purge sites with close-before-wipe; second-instance read-only; kill-process/disk-full/two-instance tests.
- P2 (12-15d): read path, the VRAM win — index probe beside loadFromFastCache; async-only blob reads; explicit-format lifecycle fix; source-components injection; encode-time pickmask/alpha at parity resolution; raw-callback→J2C forcing; NPOT guard; tombstone+fallthrough on read failure; downscale exemption; telemetry. FIRST USER-VISIBLE BUILD (~5-7 weeks in).
- P3 (4-6d): bounded steady state — SSBC7CacheSize; lax watermark eviction (~64 MB least-recently-referenced drop batches) + compaction; VRAM-pressure down-rez via RAM tail mips; multi-day soak. (Re-encode debounce deleted — records immutable under full-res-only.)
- P4 (5-7d, CORE — primary fill mechanism) IMPLEMENTED 2026-08-27, see "Promotion engine (P4, as built)" above: promotion engine — drain the want-list in preference order: fused encodes over complete J2C first, then throttled low-priority network-completion fetches for manifest-referenced / recently-used partials; paused during teleport/login; bandwidth-capped.
- P5 (7-10d): manifests + warm-up + semantic eviction pins (grid-scoped names, VOCache DP + cacheFullUpdate + processAvatarAppearance + TP-request hooks, GLTF extras coverage, deleted on cache clear). REGION HALF IMPLEMENTED 2026-08-28, see "Region manifests (P5, as built)" above: the manifest format, the recorder, the bounded arrival pre-warm and the directory LRU. Still outstanding: the ROC DP-blob miner, agent manifests at processAvatarAppearance, the location-TP-request warm-up, and semantic eviction reading manifest last_seen as an evictability pin.

CORE (P0-P4, promotion engine included since it is the fill mechanism): 33-43 dev-days ≈ 7-9 calendar weeks solo. With P5 manifests: ~40-53 days. DEFERRED behind triggers: unified all-asset store (keep the Strata container/journal spec as reference: per-tier volumes ≤1 GiB, journal+checkpoint, extent allocator, segment_mask for mesh) and the orchestration rewrite (keep the staged-rewrite-behind-facade plan as template).

## Owner calls

1. Agent-manifest privacy posture: deletion-on-clear only (mandatory regardless) vs obfuscation vs opt-in.
2. Server bakes in the BC7 tier: exclude v1 (recommended) vs include flagged evict-first (crowded-venue re-sighting speed vs churn).
3. Disk footprint: own SSBC7CacheSize slider (default 2-4 GB) vs folding into CacheSize; prefs UI honesty about ~2-4x growth. (Owner 2026-08-27: disk cost acceptable, VRAM is the priority — still pick the slider default after measuring the real ratio.)
4. ~~Cold-start posture~~ RESOLVED 2026-08-27: network promotion ON by default, throttled (owner mandate — full-res fetch is how the tier fills).
5. RenderCompressTextures: retire when BC7 ships vs keep both paths.
6. North star: is the unified all-asset container the destination (stay format-compatible; adopt container discipline where free, zero speculative generality) or is a permanent BC7 sidecar acceptable?
7. P5 timing: ship manifests right after P3, or hold for P2-P3 hit-rate telemetry.

## Open questions (owner answers folded 2026-08-27; remaining ones are measurement gates)

1. OPEN (owner: "idk" — measure): bc7enc_rdo Mpix/s on target hardware at chosen quality (designs disagreed 5-10x; gates every pool/backpressure constant and the ISPC swap decision). P0 exit criterion.
2. OPEN (measure), stakes lowered by owner: VRAM pressure is the priority and disk cost is acceptable — the real BC7:J2C byte ratio now only tunes the SSBC7CacheSize default, it no longer gates the design.
3. MOOT (owner + full-res-only): BC7 records are always discard 0, so every hit serves any desired discard via prefix read; the stored-vs-desired-discard profile no longer exists. Replaced by plain hit-rate telemetry in P2. J2C-side discard behavior stays with the existing texture pipeline ("let the texture manager figure that out").
4. APPROVED (owner: "sure"): prototype the BC7-preview → J2C-delta → re-encode loop for perceived pop/blur walking toward buildings (most user-visible behavior in the system).
5. RESOLVED (owner): no-BPTC GPUs just fall back to the J2C path — no blocklist engineering; support-rate telemetry only.
6. APPROVED (owner: "okay"): manifest miner must include GLTF extras / material-asset textures on PBR regions.
7. RESOLVED (owner): SSD-class assumed — this targets gaming desktops; HDD posture dropped from scope.
8. OPEN: sleepy_robin idle-wakeup cost at chosen pool width.
9. OPEN: mip/alpha parity — sRGB-space box filter + alpha_is_mask at small mips vs GPU mips on cutout content (vegetation, fences).
