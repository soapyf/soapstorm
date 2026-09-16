/**
 * @file ssbc7serve.cpp
 * @brief Squeeze BC7 read path - probes the sidecar store, reads a mip prefix off the reader pool and uploads it in place of a J2C decode, see doc/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssbc7serve.h"

#include "llappviewer.h"
#include "llevents.h"
#include "llgl.h"
#include "llimagegl.h"
#include "llsd.h"
#include "lltexturecache.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "ssbc7encoder.h"
#include "ssbc7store.h"
#include "threadpool.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    // Bounded for the same reason the encode queue is: WorkQueue::post BLOCKS when the queue is full, and the frame thread is the one posting here, so this module only ever uses tryPost and treats a refusal as "ask again next frame".
    const size_t SSBC7_READ_QUEUE_CAPACITY = 256;

    // How often the running tally is written out. The owner has no instrument for this feature other than the log, so a line has to appear on its own without anyone asking - but only while something is actually changing, or a quiet session becomes a wall of identical lines.
    const U32 SSBC7_SERVE_LOG_SECONDS = 60;

    const char* s_serve_verdict_names[SSBC7_SERVE_VERDICT_COUNT] =
    {
        "hit", "read queued", "uploaded",
        "feature off", "store not ready", "no record",
        "excluded fetch type", "sculpt", "ui", "icon or thumbnail", "local file",
        "caller demanded a format", "needs aux channels", "needs the raw image",
        "has alpha and no pick mask", "unsupported geometry", "reader busy",
        "blob read failed", "went stale before upload", "gl upload failed",
        "stepped back to J2C"
    };

    // One finished read waiting for the frame thread. The blob is OWNED here and moved in from the worker: parking it on the LLViewerFetchedTexture would put a multi-megabyte buffer on an object that is not thread safe and whose raw-image members are already contended.
    struct Completion
    {
        LLViewerFetchedTexture* mTex = nullptr;   // holds one reference taken on the main thread before the post, released by the pump
        LLUUID                  mUUID;
        std::vector<U8>         mBlob;
        std::vector<U8>         mPickMask;    // <SS:Nexii/> Squeeze pick masks - read from the tail of the blob in the same open; empty for an opaque texture or a record without one
        U16                     mWidth = 0;
        U16                     mHeight = 0;
        U16                     mFlags = 0;
        U8                      mMipCount = 0;
        U8                      mSrcComponents = 0;
        S32                     mServeDiscard = 0;
        bool                    mOk = false;
    };

    // A separate, NORMALLY prioritised pool. The encode pool's workers run under THREAD_MODE_BACKGROUND_BEGIN, which on Windows deprioritises disk IO as well as CPU - exactly wrong for a read that gates a visible texture - and its queue is shared with multi-hundred-millisecond encodes, so a read would be scheduled last and queued behind them. The "no second thread pool" note on the encode side is about LL::ThreadPool's by-NAME instance tracker, which a differently named pool does not collide with.
    class SSBC7ReadPool : public LL::ThreadPool
    {
    public:
        SSBC7ReadPool(size_t threads)
        :   LL::ThreadPool("BC7Read", threads, SSBC7_READ_QUEUE_CAPACITY, false)
        {}
    };

    struct ServeState
    {
        std::atomic<bool>            mEnabled{false};
        std::atomic<bool>            mGpuSupported{false};
        std::atomic<SSBC7ReadPool*>  mPool{nullptr};
        SSBC7Store*                  mStore{nullptr};   // resolved on the main thread at init, because a worker must never be the first toucher of an LLSingleton

        std::atomic<bool>            mShuttingDown{false};
        std::atomic<bool>            mAbandon{false};
        std::atomic<S32>             mReadsInFlight{0};
        std::atomic<U32>             mVerdicts[SSBC7_SERVE_VERDICT_COUNT];

        // The live gauge, which is the number the whole feature exists to move. Cumulative counters answer "did it ever work"; these answer "is it working right now".
        // <SS:Nexii/> Bytes actually moved off the disk by the reader pool. The verdict counters say how many reads were issued and none of them say how large those reads were, which left this path - the one read-heavy, write-free path in the tier that scales with camera movement - impossible to confirm or clear against a disk-usage reading. Written only by pool workers on a successful read, read by the overlay.
        std::atomic<S64>             mReadBytesTotal{0};
        std::atomic<S64>             mResidentCount{0};
        std::atomic<S64>             mResidentBC7Bytes{0};
        std::atomic<S64>             mResidentSavedBytes{0};
        std::atomic<S64>             mLifetimeSavedBytes{0};

        std::mutex                   mDoneMutex;
        std::vector<Completion>      mDone;

        std::atomic<U32>             mLastLogSecond{0};
        std::atomic<U32>             mLastLogSignature{0};

        // The first serve of a session gets its own line rather than waiting up to a minute for the tick. This is the single most important thing the log can say - the whole feature was write-only until this fired - and it should not be something the owner has to wait for.
        std::atomic<bool>            mLoggedFirstUpload{false};

        ServeState()
        {
            for (U32 i = 0; i < SSBC7_SERVE_VERDICT_COUNT; ++i) mVerdicts[i] = 0;
        }
    };

    // Created once on the main thread and then never destroyed, exactly as the encode state is: a worker that loads this pointer while the viewer is quitting has to find a closed pool rather than freed memory.
    std::atomic<ServeState*> s_state{nullptr};

    ServeState* state() { return s_state.load(std::memory_order_acquire); }

    ESSBC7ServeVerdict record(ServeState* st, ESSBC7ServeVerdict verdict)
    {
        ++st->mVerdicts[verdict];
        return verdict;
    }

    // Every exclusion the design lists, tested on the texture object rather than on the record, and re-tested at the moment of upload because almost all of them can latch LATE: setLoadedCallback turns on mNeedsAux and mSaveRawImage long after residency, and setForSculpt can arrive at any time.
    ESSBC7ServeVerdict checkTextureExclusions(LLViewerFetchedTexture* tex)
    {
        if (!tex) return SSBC7_SERVE_DECLINE_STALE;

        // Keyed on the object's own type rather than on the list it was filed under, because records are keyed by plain uuid while texture objects are keyed by (uuid, ETexListType) - the same uuid is reachable through a differently typed object.
        const S8 type = tex->getType();
        if (type != LLViewerTexture::FETCHED_TEXTURE && type != LLViewerTexture::LOD_TEXTURE)
        {
            return SSBC7_SERVE_DECLINE_FTTYPE;
        }

        if (tex->getFTType() != FTT_DEFAULT) return SSBC7_SERVE_DECLINE_FTTYPE;

        // The caller demanded a GL format at construction. Latched then and not re-tested through getHasExplicitFormat(), because once this path sets BPTC itself that flag can no longer tell "the caller asked" from "we chose".
        if (tex->ssBC7HadExplicitFormat()) return SSBC7_SERVE_DECLINE_EXPLICIT;

        // Two independent tests on purpose: setForSculpt can arrive after residency, and it raises the boost level as well as the flag.
        if (tex->forSculpt() || tex->getBoostLevel() == LLGLTexture::BOOST_SCULPTED) return SSBC7_SERVE_DECLINE_SCULPT;

        if (tex->getBoostLevel() == LLGLTexture::BOOST_UI) return SSBC7_SERVE_DECLINE_UI;

        // Keyed on BOOST and not on ETexListType because the destructive rescales in saveRawImage and loadFromFastCache key off boost directly, and a full-res upload under either one fights that rescale.
        if (tex->getBoostLevel() == LLGLTexture::BOOST_ICON || tex->getBoostLevel() == LLGLTexture::BOOST_THUMBNAIL) return SSBC7_SERVE_DECLINE_ICON;

        // The same test preCreateTexture uses, and it matters because that branch rescales the raw in place.
        const std::string& url = tex->getUrl();
        if (url.size() >= 7 && url.compare(0, 7, "file://") == 0) return SSBC7_SERVE_DECLINE_LOCAL;

        if (tex->needsAux()) return SSBC7_SERVE_DECLINE_AUX;

        // getCurrentDiscardLevelForFetching folds mForceToSaveRawImage into the current discard specifically so a raw consumer forces a refetch, so residency has to step back to the J2C path here rather than let the GL discard claim the requirement is already met.
        if (tex->needsToSaveRawImage() || tex->ssBC7NeedsRawCallback()) return SSBC7_SERVE_DECLINE_RAW;

        return SSBC7_SERVE_HIT;
    }

    // The record-side half. Separate from the texture side because the record is not in hand at every call site, and because these are the checks that assert the upload contract rather than express a policy.
    ESSBC7ServeVerdict checkRecordExclusions(ServeState* st, const SSBC7Record& rec)
    {
        if (rec.mFormat != SSBC7_FMT_BC7_UNORM) return SSBC7_SERVE_DECLINE_GEOMETRY;
        if (rec.mMipCount == 0 || rec.mMipCount > SSBC7_MAX_MIPS) return SSBC7_SERVE_DECLINE_GEOMETRY;
        if (rec.mWidth < 4 || rec.mHeight < 4) return SSBC7_SERVE_DECLINE_GEOMETRY;
        if ((rec.mWidth & (rec.mWidth - 1)) != 0 || (rec.mHeight & (rec.mHeight - 1)) != 0) return SSBC7_SERVE_DECLINE_GEOMETRY;
        if (rec.mSrcComponents < 1 || rec.mSrcComponents > 4) return SSBC7_SERVE_DECLINE_GEOMETRY;

        // PICK MASKS ARE NOT STORED YET. The encode worker never fills SSBC7Encoded::mPickMask, so a BC7 resident has none, and LLImageGL::getMask returns TRUE unconditionally in that case - which picks the whole quad of every cutout instead of its visible texels. A texture with no real alpha never had a pick mask under the ordinary path either, so serving those regresses nothing; everything else waits for the encode side to fill the mask.
        // <SS:Nexii> Squeeze pick masks - SINCE 2026-09-09 THEY ARE STORED. The encode worker builds one from the decoded base level for every texture with something transparent, the store keeps it after the payload, the read path fetches it in the same open as the prefix, and LLImageGL::ssSetPickMask installs it after the upload. So an alpha texture is served whenever its record carries a mask, and declined only when it does not - a record from before masks were stored, or one whose mask failed its checksum - because serving that one would pick as a whole quad. The old SSSqueezeServeAlpha override is gone with the limitation it measured.
        if ((rec.mFlags & (SSBC7_FLAG_FULLY_OPAQUE | SSBC7_FLAG_HAS_PICKMASK)) == 0)
        {
            return SSBC7_SERVE_DECLINE_ALPHA;
        }
        // </SS:Nexii>

        return SSBC7_SERVE_HIT;
    }

    // Store-order levels needed to serve a given discard: the base level plus every coarser one, which is a contiguous prefix precisely because the payload is written smallest first.
    U32 levelsForDiscard(U32 mip_count, S32 discard)
    {
        if (discard < 0) discard = 0;
        if ((U32)discard >= mip_count) discard = (S32)mip_count - 1;
        return mip_count - (U32)discard;
    }

    S64 uncompressedBytes(U32 width, U32 height, U32 components)
    {
        // One byte per channel, which is what LLImageGL's own accounting charges for GL_RGBA, GL_RGB, GL_LUMINANCE_ALPHA and GL_LUMINANCE alike.
        return (S64)width * (S64)height * (S64)components;
    }

    void maybeLogTally(ServeState* st)
    {
        const U32 now = ssBC7NowSeconds();
        const U32 last = st->mLastLogSecond.load();
        if (last && now >= last && (now - last) < SSBC7_SERVE_LOG_SECONDS) return;

        // Cheap change detector so a session where nothing is being served does not produce an identical line every minute. Uploads plus declines is enough: any of them moving means the picture changed.
        U32 signature = 0;
        for (U32 i = 0; i < SSBC7_SERVE_VERDICT_COUNT; ++i) signature += st->mVerdicts[i].load();

        st->mLastLogSecond.store(now);
        if (signature == st->mLastLogSignature.load()) return;
        st->mLastLogSignature.store(signature);

        LL_INFOS("Squeeze") << ssBC7ServeMetricsString() << LL_ENDL;

        // The store's own tally has never been written out during a session - it only ever appeared at startup - and the two numbers are only meaningful next to each other: records held versus records served is the difference between "the encoder is working" and "the feature is working".
        if (st->mStore && st->mStore->isInitialized())
        {
            LL_INFOS("Squeeze") << st->mStore->metricsString() << LL_ENDL;
        }
    }
}

const char* ssBC7ServeVerdictName(ESSBC7ServeVerdict verdict)
{
    if ((U32)verdict >= (U32)SSBC7_SERVE_VERDICT_COUNT) return "unknown";
    return s_serve_verdict_names[verdict];
}

void ssBC7ServeRecord(ESSBC7ServeVerdict verdict)
{
    ServeState* st = state();
    if (!st) return;
    record(st, verdict);
}

bool ssBC7ServeEnabled()
{
    ServeState* st = state();
    return st && st->mEnabled.load() && st->mGpuSupported.load() && !st->mShuttingDown.load();
}

void ssBC7ServeRefreshPolicy()
{
    ServeState* st = state();
    if (!st) return;

    const bool was_enabled = st->mEnabled.load();

    st->mEnabled      = gSavedSettings.getBOOL("SSSqueezeEnabled") && gSavedSettings.getBOOL("SSSqueezeReadEnabled");
    st->mGpuSupported = LLImageGL::canUseSqueeze();

    // <SS:Nexii> Turning serving OFF now actually takes effect on what is already on screen, which it did not before: flipping mEnabled stopped NEW serves while every texture already uploaded as BC7 stayed exactly where it was. That made the setting useless as the one thing it most needs to be - a live A/B for "is Squeeze causing what I am looking at" - because the artefact under investigation would still be there after switching it off.
    //
    // Handing each resident back through the ordinary decline path rather than inventing a teardown: dropCompressedFormat re-derives an uncompressed format and destroyTexture releases the name, after which the stock fetch path owns the texture again and re-requests it at whatever discard it wants. Cost is one walk of the texture list and a re-fetch of what was resident, paid only on a transition to off.
    if (was_enabled && !st->mEnabled.load())
    {
        U32 released = 0;
        for (const LLPointer<LLViewerFetchedTexture>& imagep : gTextureList)
        {
            LLViewerFetchedTexture* tex = imagep.get();
            if (tex && tex->ssBC7IsResident())
            {
                tex->ssBC7SetDeclined((U8)SSBC7_SERVE_DECLINE_OFF);
                ++released;
            }
        }

        LL_INFOS("Squeeze") << "BC7 serving switched off: " << released
                            << " resident textures handed back to the ordinary path, so the change is visible immediately" << LL_ENDL;
    }

    LL_INFOS("Squeeze") << "BC7 read policy: serving " << (st->mEnabled.load() ? "on" : "off")
                        << ", gpu " << (st->mGpuSupported.load() ? "supports BC7" : "has no BC7, nothing will ever be served")
                        << ", alpha textures served when their record carries a pick mask"
                        << LL_ENDL;

    if (!st->mEnabled || !st->mGpuSupported || st->mShuttingDown) return;

    // Brought up on demand for the same reason the encode side does it: Squeeze ships off, so the first time anyone tries it is by ticking the box, and needing a restart for that to take effect would make the honest description of the feature "it does not work". Done here as well as there because reading and encoding are separate settings, and a user who reads without encoding would otherwise find the store never came up at all.
    if (st->mStore && !st->mStore->isInitialized() && LLAppViewer::getTextureCache())
    {
        st->mStore->initStore(LLAppViewer::getTextureCache()->getTexturesDirName(), ssBC7EncoderVersion());
    }

    if (!st->mPool.load())
    {
        size_t threads = (size_t)gSavedSettings.getU32("SSSqueezeReadThreads");
        if (threads == 0)
        {
            // Narrow on purpose. This is IO latency hiding, not throughput: two workers are enough to keep a request in flight while another is in the kernel, and every extra one is a thread competing with the texture fetcher for the same disk.
            threads = 2;
        }
        threads = (size_t)llclamp((S32)threads, 1, 4);

        SSBC7ReadPool* pool = new SSBC7ReadPool(threads);
        pool->start();
        st->mPool.store(pool);   // published only after start(), so anyone who ever sees a non-null pool sees one with workers already servicing it

        LL_INFOS("Squeeze") << "BC7 reader pool started with " << pool->getWidth()
                            << " workers, queue capacity " << SSBC7_READ_QUEUE_CAPACITY << LL_ENDL;
    }
}

void ssBC7ServeInit()
{
    if (state()) return;

    ServeState* st = new ServeState();
    st->mStore = SSBC7Store::getInstance();
    s_state.store(st, std::memory_order_release);

    // Ours rather than the ThreadPool's built-in listener, for the same reason the encode side owns its own: the abandon flag has to be raised before the queue is closed, and the built-in one only knows how to close.
    LLEventPumps::instance().obtain("LLApp").listen(
        "SSBC7Serve",
        [](const LLSD& stat)
        {
            if (std::string(stat["status"]) != "running")
            {
                ssBC7ServeBeginShutdown();
                ssBC7ServeShutdown();
            }
            return false;
        });

    ssBC7ServeRefreshPolicy();
}

void ssBC7ServeBeginShutdown()
{
    ServeState* st = state();
    if (!st) return;

    st->mShuttingDown = true;
    st->mAbandon = true;
}

void ssBC7ServeShutdown()
{
    ServeState* st = state();
    if (!st) return;

    st->mShuttingDown = true;
    st->mAbandon = true;

    SSBC7ReadPool* pool = st->mPool.exchange(nullptr);
    if (pool)
    {
        // close() drains and then joins. Every remaining item sees the abandon flag and returns immediately after pushing its completion, so this costs one in-progress read rather than the whole backlog.
        pool->close();
    }

    // Every completion still holds a texture reference taken on the main thread, and the pump that would have released it is never going to run again.
    std::vector<Completion> leftovers;
    {
        std::lock_guard<std::mutex> lock(st->mDoneMutex);
        leftovers.swap(st->mDone);
    }
    for (Completion& c : leftovers)
    {
        if (c.mTex) c.mTex->unref();
    }

    LL_INFOS("Squeeze") << "BC7 reader pool stopped. " << ssBC7ServeMetricsString() << LL_ENDL;
}

ESSBC7ServeVerdict ssBC7ServeProbe(LLViewerFetchedTexture* tex)
{
    ServeState* st = state();
    if (!st) return SSBC7_SERVE_DECLINE_NOT_READY;
    if (!tex) return SSBC7_SERVE_DECLINE_STALE;

    if (!st->mEnabled.load() || !st->mGpuSupported.load() || st->mShuttingDown.load())
    {
        // Not recorded as a decline: with the feature off this would fire for every texture the viewer ever creates and would drown every other count in the tally.
        return SSBC7_SERVE_DECLINE_OFF;
    }

    if (!st->mStore || !st->mStore->isInitialized())
    {
        // Deliberately not counted and deliberately not latched as DECLINED. The store coming up is a transient startup condition rather than a decision about this texture, so the ladder stays at NONE and the safety-net probe in updateFetch asks again once it is up; counting it would put a seven figure number against a verdict that means nothing.
        return SSBC7_SERVE_DECLINE_NOT_READY;
    }

    const ESSBC7ServeVerdict excluded = checkTextureExclusions(tex);
    if (excluded != SSBC7_SERVE_HIT)
    {
        tex->ssBC7SetDeclined((U8)excluded);
        return record(st, excluded);
    }

    // lookup rather than hasRecord: it returns the whole record under the same single map-mutex acquisition with no IO, which is everything needed to size the GL texture before the blob arrives, and it stamps the heat signal so a read-served texture never ranks cold in eviction.
    SSBC7Record rec;
    if (!st->mStore->lookup(tex->getID(), rec))
    {
        tex->ssBC7SetDeclined((U8)SSBC7_SERVE_DECLINE_NO_RECORD);
        return record(st, SSBC7_SERVE_DECLINE_NO_RECORD);
    }

    const ESSBC7ServeVerdict rec_verdict = checkRecordExclusions(st, rec);
    if (rec_verdict != SSBC7_SERVE_HIT)
    {
        tex->ssBC7SetDeclined((U8)rec_verdict);
        return record(st, rec_verdict);
    }

    tex->ssBC7SetResidency((U8)LLViewerFetchedTexture::SSBC7_RES_HIT_KNOWN);
    return record(st, SSBC7_SERVE_HIT);
}

ESSBC7ServeVerdict ssBC7ServeRequest(LLViewerFetchedTexture* tex, S32 desired_discard)
{
    ServeState* st = state();
    if (!st) return SSBC7_SERVE_DECLINE_NOT_READY;
    if (!tex) return SSBC7_SERVE_DECLINE_STALE;

    if (!st->mEnabled.load() || !st->mGpuSupported.load() || st->mShuttingDown.load())
    {
        return SSBC7_SERVE_DECLINE_OFF;
    }

    const U8 ladder = tex->ssBC7Residency();
    if (ladder == LLViewerFetchedTexture::SSBC7_RES_NONE
        || ladder == LLViewerFetchedTexture::SSBC7_RES_DECLINED
        || ladder == LLViewerFetchedTexture::SSBC7_RES_READING)
    {
        // READING is exactly what stops a second probe queueing a second read for the same texture.
        return SSBC7_SERVE_DECLINE_BUSY;
    }

    if (!st->mStore || !st->mStore->isInitialized())
    {
        // Transient rather than a decision about this texture, so the ladder is left where it is and the next pass asks again - latching DECLINED here would strand every texture that happened to be wanted while the store was still coming up.
        return record(st, SSBC7_SERVE_DECLINE_NOT_READY);
    }

    const ESSBC7ServeVerdict excluded = checkTextureExclusions(tex);
    if (excluded != SSBC7_SERVE_HIT)
    {
        tex->ssBC7SetDeclined((U8)excluded);
        return record(st, excluded);
    }

    SSBC7Record rec;
    if (!st->mStore->lookup(tex->getID(), rec))
    {
        // The record was here at probe time and is not here now, which means eviction or a failed verify dropped it. That is permanent for this session.
        tex->ssBC7SetDeclined((U8)SSBC7_SERVE_DECLINE_NO_RECORD);
        return record(st, SSBC7_SERVE_DECLINE_NO_RECORD);
    }

    const ESSBC7ServeVerdict rec_verdict = checkRecordExclusions(st, rec);
    if (rec_verdict != SSBC7_SERVE_HIT)
    {
        tex->ssBC7SetDeclined((U8)rec_verdict);
        return record(st, rec_verdict);
    }

    const U32 levels = levelsForDiscard(rec.mMipCount, desired_discard);
    const S32 serve_discard = (S32)rec.mMipCount - (S32)levels;

    if (tex->ssBC7Residency() == LLViewerFetchedTexture::SSBC7_RES_RESIDENT && tex->ssBC7ServedDiscard() == serve_discard)
    {
        return SSBC7_SERVE_DECLINE_BUSY;   // already exactly what was asked for
    }

    SSBC7ReadPool* pool = st->mPool.load();
    if (!pool)
    {
        // Also transient - the pool is brought up on demand when the settings are refreshed - so the ladder keeps its place and the next pass asks again.
        return record(st, SSBC7_SERVE_DECLINE_NOT_READY);
    }

    SSBC7Store* store = st->mStore;
    const LLUUID id = tex->getID();

    // The reference is taken HERE, on the main thread, and released by the pump. LLRefCount is not thread safe, so the worker only ever holds the pointer; it never touches the count. This is the same shape scheduleCreateTexture uses for its off-thread create.
    tex->ref();
    ++st->mReadsInFlight;
    tex->ssBC7SetResidency((U8)LLViewerFetchedTexture::SSBC7_RES_READING);

    const bool posted = pool->getQueue().tryPost(
        [st, store, tex, id, levels, serve_discard]()
        {
            Completion done;
            done.mTex          = tex;
            done.mUUID         = id;
            done.mServeDiscard = serve_discard;

            if (!st->mAbandon.load())
            {
                SSBC7Record rec2;
                if (store->readBlobPrefix(id, levels, done.mBlob, rec2, &done.mPickMask))
                {
                    st->mReadBytesTotal += (S64)done.mBlob.size();   // <SS:Nexii/> see mReadBytesTotal
                    done.mWidth         = rec2.mWidth;
                    done.mHeight        = rec2.mHeight;
                    done.mFlags         = rec2.mFlags;
                    done.mMipCount      = rec2.mMipCount;
                    done.mSrcComponents = rec2.mSrcComponents;
                    done.mOk            = true;
                }
            }

            // Pushed unconditionally, including on the abandon path, because the pump is the only thing that can release the reference taken above.
            {
                std::lock_guard<std::mutex> lock(st->mDoneMutex);
                st->mDone.push_back(std::move(done));
            }
            --st->mReadsInFlight;
        });

    if (!posted)
    {
        // tryPost returns false when the queue is full OR closed, and neither one runs the work item, so everything taken above has to come straight back or the reference and the in-flight count leak for the rest of the session.
        --st->mReadsInFlight;
        tex->ssBC7SetResidency((U8)(tex->ssBC7ServedDiscard() >= 0
                                    ? LLViewerFetchedTexture::SSBC7_RES_RESIDENT
                                    : LLViewerFetchedTexture::SSBC7_RES_HIT_KNOWN));
        tex->unref();
        return record(st, SSBC7_SERVE_DECLINE_BUSY);
    }

    return record(st, SSBC7_SERVE_QUEUED);
}

// <SS:Nexii/> Cumulative bytes the reader pool has pulled off the disk this session. The overlay differences it against wall time to get a rate; keeping the raw total here rather than a rate means no clock lives on the pool threads.
S64 ssBC7ServeReadBytesTotal()
{
    ServeState* st = state();
    return st ? st->mReadBytesTotal.load() : 0;
}

// <SS:Nexii> Squeeze region manifests - additive accessor over the counter the reader pool already maintains, so a background pre-warm can refuse to post while the demand path has work outstanding. Nothing in the read path reads this; it exists only so the manifest pass can lose the race on purpose.
S32 ssBC7ServeReadsInFlight()
{
    ServeState* st = state();
    return st ? st->mReadsInFlight.load() : 0;
}
// </SS:Nexii>

F32 ssBC7ServePumpUploads(F32 max_time)
{
    ServeState* st = state();
    if (!st) return 0.f;

    maybeLogTally(st);

    {
        std::lock_guard<std::mutex> lock(st->mDoneMutex);
        if (st->mDone.empty()) return 0.f;
    }

    LLTimer timer;

    for (;;)
    {
        Completion done;
        {
            std::lock_guard<std::mutex> lock(st->mDoneMutex);
            if (st->mDone.empty()) break;
            done = std::move(st->mDone.back());
            st->mDone.pop_back();
        }

        LLViewerFetchedTexture* tex = done.mTex;
        if (!tex)
        {
            continue;
        }

        ESSBC7ServeVerdict verdict = SSBC7_SERVE_UPLOADED;

        if (!done.mOk)
        {
            verdict = SSBC7_SERVE_DECLINE_READ;
        }
        else if (st->mShuttingDown.load())
        {
            verdict = SSBC7_SERVE_DECLINE_STALE;
        }
        else
        {
            // RE-VALIDATED AT THE MOMENT OF UPLOAD. Between the post and here the texture can have been sculpt-flagged, boosted to an icon, or handed a raw-needing callback, and every one of those makes serving wrong rather than merely suboptimal.
            const ESSBC7ServeVerdict excluded = checkTextureExclusions(tex);
            if (excluded != SSBC7_SERVE_HIT)
            {
                verdict = excluded;
            }
            else if (tex->ssBC7Residency() != LLViewerFetchedTexture::SSBC7_RES_READING)
            {
                // Something took the texture off the ladder while the read was in flight - a raw consumer, a destroy, a J2C upgrade. It already recorded a more specific reason than this one, so the blob is dropped without overwriting it.
                verdict = SSBC7_SERVE_DECLINE_STALE;
                record(st, verdict);
                tex->unref();
                if (timer.getElapsedTimeF32() > max_time) break;
                continue;
            }
            else if (tex->isForSculptOnly() || tex->ssBC7CreateInFlight() || tex->getGLTexture() == nullptr)
            {
                // An uncompressed create is already queued or in flight for this texture. Uploading BC7 underneath it would at best be overwritten a frame later, and on the LLImageGLThread route would be two threads writing the same LLImageGL.
                verdict = SSBC7_SERVE_DECLINE_STALE;
            }
        }

        if (verdict == SSBC7_SERVE_UPLOADED)
        {
            const U32 levels   = (U32)done.mMipCount - (U32)done.mServeDiscard;
            const U32 prefix   = ssBC7PrefixBytes(done.mWidth, done.mHeight, done.mMipCount, levels);
            const U32 base_off = ssBC7LevelOffset(done.mWidth, done.mHeight, done.mMipCount, levels - 1);

            if (prefix == 0 || done.mBlob.size() < (size_t)SSBC7_BLOB_HEADER_SIZE + prefix)
            {
                verdict = SSBC7_SERVE_DECLINE_READ;
            }
            else
            {
                // The pointer handed to createGLTexture is the START of the LARGEST level present, because setImage walks data_in BACKWARD one level at a time from mCurrentDiscardLevel down to mMaxDiscardLevel.
                const U8* data_in = done.mBlob.data() + SSBC7_BLOB_HEADER_SIZE + base_off;

                // <SS:Nexii> A FULLY OPAQUE texture IS an alpha mask as far as the renderer is concerned, and reading only the ALPHA_IS_MASK flag here quietly said otherwise for every texture this feature serves by default.
                //
                // The store's two flags are mutually exclusive - classifyAlpha sets FULLY_OPAQUE when nothing is transparent and ALPHA_IS_MASK only otherwise - but LLImageGL::analyzeAlpha does not partition them that way. Work its arithmetic on an all-255 alpha channel: every sample lands in the top bucket so there is no midrange, and its disqualifier alphatotal != 255*length is false because a four component image counts two samples per texel. Stock therefore sets mIsMask TRUE on a fully opaque RGBA texture, and mIsMask is what LLFace::canRenderAsMask reads to choose between the deferred alpha-mask pass and forward blending.
                //
                // Getting this wrong is invisible and expensive: the texture still looks broadly right, but it renders blended instead of masked, loses its depth write, and leaves the deferred path. And FULLY_OPAQUE is not an edge case here - it is most of the population being served.
                //
                // Components one and three carry no alpha at all and must stay false, which is the same line calcAlphaChannelOffsetAndStride draws when it returns early for GL_LUMINANCE and GL_RGB.
                const bool has_alpha_channel = (done.mSrcComponents == 2 || done.mSrcComponents == 4);
                const bool alpha_is_mask = has_alpha_channel
                                           && ((done.mFlags & (SSBC7_FLAG_ALPHA_IS_MASK | SSBC7_FLAG_FULLY_OPAQUE)) != 0);

                if (tex->ssBC7UploadFromStore(data_in, done.mServeDiscard, (S32)done.mWidth, (S32)done.mHeight,
                                              (S32)done.mSrcComponents, (S32)done.mMipCount, alpha_is_mask,
                                              done.mPickMask.empty() ? nullptr : done.mPickMask.data(), (U32)done.mPickMask.size()))
                {
                    const S32 w = (S32)(done.mWidth  >> done.mServeDiscard) ? (S32)(done.mWidth  >> done.mServeDiscard) : 1;
                    const S32 h = (S32)(done.mHeight >> done.mServeDiscard) ? (S32)(done.mHeight >> done.mServeDiscard) : 1;

                    // Base level only, matching the "Does not include mipmaps" contract on getTextureBytesAllocated, so this number is directly comparable with what the memory governor sees.
                    const S64 bc7_bytes  = (S64)ssBC7LevelBytes((U32)w, (U32)h);
                    const S64 flat_bytes = uncompressedBytes((U32)w, (U32)h, (U32)done.mSrcComponents);
                    const S64 saved      = llmax((S64)0, flat_bytes - bc7_bytes);

                    tex->ssBC7NoteResident(done.mServeDiscard, (U32)bc7_bytes, (U32)saved);

                    ++st->mResidentCount;
                    st->mResidentBC7Bytes   += bc7_bytes;
                    st->mResidentSavedBytes += saved;
                    st->mLifetimeSavedBytes += saved;

                    if (!st->mLoggedFirstUpload.exchange(true))
                    {
                        LL_INFOS("Squeeze") << "first BC7 texture served from the store: " << done.mUUID
                                            << " at " << w << "x" << h << " (full " << done.mWidth << "x" << done.mHeight
                                            << ", discard " << done.mServeDiscard << ", " << (U32)done.mSrcComponents << " source components)"
                                            << ", " << bc7_bytes << " bytes of BC7 in place of " << flat_bytes
                                            << " uncompressed, and no J2C decode" << LL_ENDL;
                    }
                }
                else
                {
                    verdict = SSBC7_SERVE_DECLINE_UPLOAD;
                }
            }
        }

        if (verdict != SSBC7_SERVE_UPLOADED)
        {
            // Every failure is silent to the user, non-fatal, and carries a name. A texture that lands here simply takes the ordinary J2C path exactly as it does today.
            tex->ssBC7SetDeclined((U8)verdict);
        }

        record(st, verdict);
        tex->unref();

        if (timer.getElapsedTimeF32() > max_time) break;
    }

    return timer.getElapsedTimeF32();
}

void ssBC7ServeNoteResidencyLost(U32 bc7_bytes, U32 saved_bytes)
{
    ServeState* st = state();
    if (!st) return;

    if (st->mResidentCount.load() > 0) --st->mResidentCount;
    st->mResidentBC7Bytes   -= (S64)bc7_bytes;
    st->mResidentSavedBytes -= (S64)saved_bytes;
    if (st->mResidentBC7Bytes.load() < 0) st->mResidentBC7Bytes = 0;
    if (st->mResidentSavedBytes.load() < 0) st->mResidentSavedBytes = 0;
}

// <SS:Nexii> The overlay's read of the same three numbers the log line formats, handed over raw so it can lay them out itself.
SSBC7ServeResidency ssBC7ServeResidencyNow()
{
    SSBC7ServeResidency out;
    ServeState* st = state();
    if (!st) return out;

    out.mBC7Bytes   = st->mResidentBC7Bytes.load();
    out.mSavedBytes = st->mResidentSavedBytes.load();
    out.mTextures   = (U32)llmax((S64)0, (S64)st->mResidentCount.load());
    return out;
}

std::string ssBC7ServeMetricsString()
{
    ServeState* st = state();
    if (!st) return "BC7 read path not started";

    std::string counts;
    for (U32 i = 0; i < SSBC7_SERVE_VERDICT_COUNT; ++i)
    {
        const U32 n = st->mVerdicts[i].load();
        if (!n) continue;
        if (!counts.empty()) counts += ", ";
        counts += llformat("%s %u", ssBC7ServeVerdictName((ESSBC7ServeVerdict)i), n);
    }
    if (counts.empty()) counts = "nothing considered yet";

    const S64 bc7   = st->mResidentBC7Bytes.load();
    const S64 saved = st->mResidentSavedBytes.load();

    return llformat("BC7 serve: %s | resident now %lld textures holding %lld MB of BC7 in place of %lld MB uncompressed, saving %lld MB of video memory right now (%lld MB saved this session in total) | reads in flight %d",
                    counts.c_str(),
                    (long long)st->mResidentCount.load(),
                    (long long)(bc7 / (1024 * 1024)),
                    (long long)((bc7 + saved) / (1024 * 1024)),
                    (long long)(saved / (1024 * 1024)),
                    (long long)(st->mLifetimeSavedBytes.load() / (1024 * 1024)),
                    (S32)st->mReadsInFlight.load());
}
