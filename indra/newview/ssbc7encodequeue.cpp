/**
 * @file ssbc7encodequeue.cpp
 * @brief Squeeze BC7 encode pool - the demand-path hook that turns a freshly decoded full resolution texture into a stored sidecar, see doc/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssbc7encodequeue.h"

#include "llappviewer.h"
#include "llevents.h"
#include "lltexturecache.h"
#include "llimage.h"
#include "llimagegl.h"
#include "llsd.h"
#include "llwin32headers.h"
#include "llviewercontrol.h"
#include "llviewertexturelist.h"   // <SS:Nexii> Squeeze eviction - the maintenance tick walks the resident set to mark what is still referenced
#include "ssbc7adaptive.h"         // <SS:Nexii/> Squeeze adaptive quality - the profile every encode is done at, and where the throughput it achieved is reported
#include "ssbc7encoder.h"
#include "ssbc7promote.h"          // <SS:Nexii/> Squeeze promotion - the bounded want list lives with the engine that drains it, so there is one implementation of "what gets dropped when it is full" rather than two
#include "ssbc7store.h"
#include "threadpool.h"

#include <atomic>
#include <chrono>                   // <SS:Nexii/> Squeeze adaptive quality - a monotonic clock for the per-encode timing, which must never be able to go backwards and report an infinite rate
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace
{
    // Bounded on purpose. The pool takes work from the texture fetch thread, and WorkQueue::post BLOCKS when the queue is full - a fetch thread parked on a BC7 queue would stall every texture in the world, so this module only ever uses tryPost and treats a refusal as a want-list entry.
    const size_t SSBC7_QUEUE_CAPACITY = 128;

    // Each queued item pins a decoded LLImageRaw alive. A 2048 square RGBA raw is 16 MB, so an unbounded queue during a teleport arrival would hold hundreds of megabytes hostage; this is the real backpressure, the item count is only a coarse first cut.
    const S64 SSBC7_MAX_PINNED_BYTES = 256ll * 1024 * 1024;

    // How often the running verdict tally is written to the log. Frequent enough that a short session still produces one line, rare enough that a busy region does not drown the log.
    const U32 SSBC7_LOG_EVERY = 512;

    const char* s_verdict_names[SSBC7_VERDICT_COUNT] =
    {
        "enqueued", "feature off", "store not ready", "excluded type", "not full resolution",
        "unsupported geometry", "already stored", "backpressure", "post refused"
    };

    // Set once on the main thread and read from the fetch thread. These are plain values rather than gSavedSettings lookups because the fetch thread must never touch the control group, and rather than LLCachedControl because its first construction would then happen on whichever thread got there first.
    struct Policy
    {
        std::atomic<bool> mEnabled{false};
        std::atomic<bool> mBackgroundEncode{true};
        std::atomic<bool> mGpuSupported{false};
        std::atomic<U32>  mMinSize{64};
    };

    class SSBC7EncodePool : public LL::ThreadPool
    {
    public:
        // auto_shutdown is false so that nothing closes this pool behind our back. The stock listener closes the queue, and a WorkQueue DRAINS before it closes - which at quit means executing every queued megapixel encode while the user waits. Shutdown here has to raise the abandon flag first, so it owns the whole sequence.
        SSBC7EncodePool(size_t threads)
        :   LL::ThreadPool("BC7Encode", threads, SSBC7_QUEUE_CAPACITY, false)
        {}

        void run() override
        {
#if LL_WINDOWS
            // Background QoS deprioritises CPU, disk and memory pressure together, which is exactly the shape of this work. Note the existing in-tree call at llappviewerwin32.cpp:1404 passes a handle to a DIFFERENT thread, and THREAD_MODE_BACKGROUND_BEGIN is only ever valid for the calling thread - so that one does nothing and this is the first place the mode is actually applied.
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
#endif
            LL::ThreadPool::run();
#if LL_WINDOWS
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
#endif
        }
    };

    struct EncodeState
    {
        Policy                          mPolicy;
        // Published only once start() has returned, and never taken down again while the session lasts. A pool is created at most once because LL::ThreadPool registers itself by name in an instance tracker, so closing one and building another under the same name would collide - which means disabling Squeeze mid-session leaves idle workers behind rather than reclaiming them, and that is the cheaper of the two wrong answers.
        std::atomic<SSBC7EncodePool*>   mPool{nullptr};
        SSBC7Store*                     mStore{nullptr};   // resolved on the main thread at init, because a worker must never be the first toucher of an LLSingleton

        std::atomic<bool>               mShuttingDown{false};
        std::atomic<bool>               mAbandon{false};
        std::atomic<bool>               mClosed{false};
        std::atomic<S32>                mPending{0};
        std::atomic<S64>                mPinnedBytes{0};
        std::atomic<U32>                mConsidered{0};
        std::atomic<U32>                mAbandoned{0};
        std::atomic<U32>                mEncodeFailed{0};
        std::atomic<U32>                mVerdicts[SSBC7_VERDICT_COUNT];

        std::mutex                      mSetMutex;
        std::unordered_set<LLUUID>      mInFlight;
        // <SS:Nexii> Squeeze promotion - a bounded list that names what it evicted, replacing the plain set that simply refused new entries once full. Refusing new ones pins the list to whatever the first eight thousand textures of the session happened to be and then reads, from a log line, exactly as though the engine had everything in hand.
        SSBC7WantList                   mWantList{SSBC7_PROMOTE_WANT_CAP};
        std::vector<LLUUID>             mFreshlyStored;   // uuids that reached the store, drained by the main thread so the read path can re-probe a texture it had already declined for having no record
        // </SS:Nexii>

        EncodeState()
        {
            for (U32 i = 0; i < SSBC7_VERDICT_COUNT; ++i) mVerdicts[i] = 0;
        }
    };

    // Created once on the main thread, then never destroyed. A fetch thread that loads this pointer while the viewer is quitting has to find a closed pool, not freed memory, and the object is a few hundred bytes.
    std::atomic<EncodeState*> s_state{nullptr};

    EncodeState* state() { return s_state.load(std::memory_order_acquire); }

    // <SS:Nexii> Squeeze promotion - one walk through a busy mall must not be able to queue an unbounded set of uuids that nothing will ever get around to, and when the cap does bite it has to SAY what it gave up. The list drops oldest-first and hands work out newest-first, so what survives a mall is the room the user is actually stood in; the drop is counted for the whole session and logged the first time and then once per further thousand, because a cap that bites on every single insert would otherwise write a line per texture.
    void noteWanted(EncodeState* st, const LLUUID& id)
    {
        LLUUID dropped;
        bool   did_drop = false;
        U32    total_dropped = 0;

        {
            std::lock_guard<std::mutex> lock(st->mSetMutex);
            st->mWantList.add(id, dropped, did_drop);
            total_dropped = st->mWantList.droppedTotal();
        }

        if (did_drop && (total_dropped == 1 || (total_dropped % 1000) == 0))
        {
            LL_WARNS("Squeeze") << "BC7 want list is at its cap of " << SSBC7_PROMOTE_WANT_CAP
                                << " textures, so " << dropped << " was dropped to make room for " << id
                                << " - " << total_dropped << " dropped this session, oldest first, and those textures will not be promoted unless they are seen again" << LL_ENDL;
        }
    }
    // </SS:Nexii>

    ESSBC7EncodeVerdict record(EncodeState* st, ESSBC7EncodeVerdict verdict)
    {
        ++st->mVerdicts[verdict];

        const U32 n = ++st->mConsidered;
        if ((n % SSBC7_LOG_EVERY) == 0)
        {
            LL_INFOS("Squeeze") << ssBC7EncodeMetricsString() << LL_ENDL;
        }
        return verdict;
    }

    // Retires the worker-side accounting. Everything here runs on the WORKER and never in a completion callback on the main thread: at quit the main loop has stopped pumping, so a counter decremented there would never come back down and the pinned-bytes budget would stay permanently exhausted. This project has already shipped that bug once in the region cache.
    struct EncodeGuard
    {
        EncodeGuard(EncodeState* st, const LLUUID& id, S64 bytes) : mState(st), mUUID(id), mBytes(bytes) {}
        ~EncodeGuard()
        {
            --mState->mPending;
            mState->mPinnedBytes -= mBytes;
            std::lock_guard<std::mutex> lock(mState->mSetMutex);
            mState->mInFlight.erase(mUUID);
        }
        EncodeState* mState;
        LLUUID       mUUID;
        S64          mBytes;
    };

    // Alpha shape, decided from the source pixels while the shared lock is still held. Cheap next to the encode itself, and doing it now is what keeps a later pass from having to re-read the whole texture just to learn whether it is opaque.
    void classifyAlpha(const U8* data, U32 width, U32 height, U32 components, U16& out_flags)
    {
        if (components != 2 && components != 4)
        {
            out_flags |= SSBC7_FLAG_FULLY_OPAQUE;
            return;
        }

        const U32 stride = components;
        const size_t texels = (size_t)width * height;
        bool any_transparent = false;
        bool any_midtone = false;

        for (size_t i = 0; i < texels; ++i)
        {
            const U8 a = data[i * stride + (components - 1)];
            if (a != 255)
            {
                any_transparent = true;
                // The renderer treats a binary cutout differently from smooth alpha, and the boundary it cares about is whether anything lands between the two extremes.
                if (a != 0) { any_midtone = true; break; }
            }
        }

        if (!any_transparent)   out_flags |= SSBC7_FLAG_FULLY_OPAQUE;
        else if (!any_midtone)  out_flags |= SSBC7_FLAG_ALPHA_IS_MASK;
    }

    // <SS:Nexii> Squeeze pick masks - the per-texel click mask, built at encode from the decoded base level and stored beside the payload, because a BC7 upload has no alpha bytes for LLImageGL::updatePickMask to read and without a mask every cutout picks as a whole quad. The layout is updatePickMask's to the bit - one bit per 2x2 cell, row major, set where alpha exceeds 32, packed LSB first - so LLImageGL::ssSetPickMask can install it unchanged; the only difference is that this is always built from the FULL base level, where the stock path builds from whatever level happened to be uploaded last. Parity resolution rather than a cap, because coarsening it changes picking on vegetation and fences, which is the case the mask exists for.
    void buildPickMask(const U8* data, U32 width, U32 height, U32 components, std::vector<U8>& out)
    {
        out.clear();
        if (components != 2 && components != 4) return;

        const U32 pick_width  = width / 2 + 1;
        const U32 pick_height = height / 2 + 1;
        out.assign(((size_t)pick_width * pick_height + 7) / 8, 0);

        const U32 stride = components;
        U32 pick_bit = 0;
        for (U32 y = 0; y < height; y += 2)
        {
            for (U32 x = 0; x < width; x += 2)
            {
                const U8 alpha = data[((size_t)y * width + x) * stride + (components - 1)];
                if (alpha > 32)
                {
                    out[pick_bit / 8] |= (U8)(1 << (pick_bit % 8));
                }
                ++pick_bit;
            }
        }
    }
    // </SS:Nexii>

    bool isPowerOfTwo(U32 v) { return v != 0 && (v & (v - 1)) == 0; }
}

const char* ssBC7VerdictName(ESSBC7EncodeVerdict verdict)
{
    if ((U32)verdict >= (U32)SSBC7_VERDICT_COUNT) return "unknown";
    return s_verdict_names[verdict];
}

void ssBC7EncodeRefreshPolicy()
{
    EncodeState* st = state();
    if (!st) return;

    st->mPolicy.mEnabled          = gSavedSettings.getBOOL("SSSqueezeEnabled");
    st->mPolicy.mBackgroundEncode = gSavedSettings.getBOOL("SSSqueezeBackgroundEncode");
    st->mPolicy.mMinSize          = gSavedSettings.getU32("SSSqueezeMinTextureSize");

    // Snapshotted here rather than read on the fetch thread: canUseSqueeze() reads gGLManager, and there is no reason to have a worker thread reaching into GL state at all. A card with no BPTC would produce records nothing could ever upload.
    st->mPolicy.mGpuSupported     = LLImageGL::canUseSqueeze();

    LL_INFOS("Squeeze") << "BC7 encode policy: enabled " << (st->mPolicy.mEnabled.load() ? 1 : 0)
                        << ", background encode " << (st->mPolicy.mBackgroundEncode.load() ? 1 : 0)
                        << ", gpu " << (st->mPolicy.mGpuSupported.load() ? "supports BC7" : "has no BC7, nothing will be encoded")
                        << ", minimum size " << st->mPolicy.mMinSize.load() << LL_ENDL;

    // Everything below brings the tier up on demand. Squeeze ships off, so the very first time anyone tries it is by ticking the box at the login screen or in preferences - if that needed a restart to take effect, the honest description of the feature would be that it does not work. This is the same lesson the region cache already learned.
    if (!st->mPolicy.mEnabled || !st->mPolicy.mBackgroundEncode || !st->mPolicy.mGpuSupported) return;
    if (st->mShuttingDown) return;

    if (st->mStore && !st->mStore->isInitialized() && LLAppViewer::getTextureCache())
    {
        st->mStore->initStore(LLAppViewer::getTextureCache()->getTexturesDirName(), ssBC7EncoderVersion());
    }

    if (!st->mPool.load())
    {
        size_t threads = (size_t)gSavedSettings.getU32("SSSqueezeEncodeThreads");
        if (threads == 0)
        {
            // <SS:Nexii> HALF the logical processors, which on any hyperthreaded machine is about the physical core count. Not a quarter, which was the old rule, and not all of them - and the reason it is neither is worth stating because "use everything" is the obvious answer and it is wrong for a specific, measurable reason.
            //
            // Width is NOT what keeps this out of the way. The pool runs at background QoS, which deprioritises CPU, disk and memory pressure together, so a wide pool yields to foreground work exactly as readily as a narrow one and merely finishes sooner when nothing is competing. Being polite is the scheduler's job here, not the width's.
            //
            // What actually stops paying is the hardware. The rule was set when this encoder was bc7e through AVX2, living in the vector units that the two logical processors on one physical core SHARE, so the second thread per core bought something like a fifth of a thread's work while costing a whole thread's worth of wakeups, cache pressure and pinned decoded image. bc7f is scalar floating point and the sharing is less severe, but the half rule stays because the other half of the argument below is now the stronger one.
            //
            // And upstream of all of it the pool is SUPPLY limited, not compute limited: at bc7f Default a single worker turns over roughly thirty-five 1024 square textures a second on textured content, so even a couple of workers can consume far more than the fetch path can deliver. Widening past this point would buy idle workers.
            //
            // Anyone who wants every thread can still say so - SSSqueezeEncodeThreads is an explicit override and this branch only runs when it is left at zero.
            const unsigned hw = std::thread::hardware_concurrency();
            threads = (size_t)llclamp((S32)(hw / 2), 2, 32);
        }

        SSBC7EncodePool* pool = new SSBC7EncodePool(threads);
        pool->start();
        st->mPool.store(pool);   // published only after start(), so a fetch thread that ever sees a non-null pool sees one with workers already servicing it

        LL_INFOS("Squeeze") << "BC7 encode pool started with " << pool->getWidth()
                            << " workers, queue capacity " << SSBC7_QUEUE_CAPACITY << LL_ENDL;
    }
}

void ssBC7EncodeInit()
{
    if (state()) return;

    EncodeState* st = new EncodeState();
    st->mStore = SSBC7Store::getInstance();
    s_state.store(st, std::memory_order_release);

    // Our own shutdown listener, so the pool is always joined even if the explicit hook in LLAppViewer::cleanup is ever moved or lost. It has to be ours rather than the ThreadPool's built-in one because the abandon flag must be raised BEFORE the queue is closed, and the built-in listener only knows how to close.
    LLEventPumps::instance().obtain("LLApp").listen(
        "SSBC7Encode",
        [](const LLSD& stat)
        {
            if (std::string(stat["status"]) != "running")
            {
                ssBC7EncodeBeginShutdown();
                ssBC7EncodeShutdown();
            }
            return false;
        });

    // No threads are created here. Squeeze ships off, and a pool of workers polling for work nobody will ever post is a cost every user who never touches the feature would otherwise pay.
    ssBC7EncodeRefreshPolicy();
}

void ssBC7EncodeBeginShutdown()
{
    EncodeState* st = state();
    if (!st) return;

    st->mShuttingDown = true;
    st->mAbandon = true;

    // The store is shut at the same instant rather than later on, so a worker already past its abandon check finds the door closed instead of appending into a store that is about to be torn down. Keeping the two in one call is what stops them from drifting apart the next time the shutdown sequence is reordered.
    if (st->mStore) st->mStore->beginShutdown();
}

void ssBC7EncodeShutdown()
{
    EncodeState* st = state();
    if (!st) return;

    st->mShuttingDown = true;
    st->mAbandon = true;

    if (st->mClosed.exchange(true)) return;

    if (SSBC7EncodePool* pool = st->mPool.load())
    {
        // close() drains the queue and then joins. Every remaining item sees the abandon flag and returns immediately, so this costs one in-progress encode rather than the whole backlog.
        pool->close();
    }

    LL_INFOS("Squeeze") << "BC7 encode pool stopped. " << ssBC7EncodeMetricsString() << LL_ENDL;
}

ESSBC7EncodeVerdict ssBC7EncodeConsider(const LLUUID& id,
                                        FTType ftype,
                                        const LLPointer<LLImageRaw>& raw,
                                        S32 decoded_discard,
                                        bool have_all_data,
                                        bool needs_aux,
                                        bool in_local_cache)
{
    EncodeState* st = state();
    if (!st) return SSBC7_DECLINE_NOT_READY;

    if (!st->mPolicy.mEnabled || !st->mPolicy.mBackgroundEncode || !st->mPolicy.mGpuSupported)
    {
        return record(st, SSBC7_DECLINE_OFF);
    }

    SSBC7EncodePool* pool = st->mPool.load();
    if (!pool || st->mShuttingDown || !st->mStore || !st->mStore->isInitialized() || st->mStore->isReadOnly())
    {
        return record(st, SSBC7_DECLINE_NOT_READY);
    }

    // Excluded kinds. Bakes churn a fresh uuid on every outfit change, map tiles and local files are not world content, and anything wanting the aux channel is a bump or sculpt source whose consumer needs the raw rather than a compressed upload. See doc/super_compressed_textures.md for the full list and the reasoning behind each.
    if (ftype != FTT_DEFAULT || needs_aux || in_local_cache)
    {
        return record(st, SSBC7_DECLINE_TYPE);
    }

    // FULL RESOLUTION ONLY. Encoding an intermediate discard spends the same CPU for a record that the next fetch supersedes, so anything short of the complete asset at discard zero becomes a want-list entry for the promotion engine instead.
    if (decoded_discard != 0 || !have_all_data || raw.isNull())
    {
        // A texture that failed to decode at all, or was blocked before it ever decoded, arrives here with no raw and a negative discard. It is not a partial asset waiting to be completed, so it stays off the want list - the promotion engine would only spend network on something that has already refused to exist.
        //
        // <SS:Nexii> And neither does anything whose FULL resolution could never pass the geometry gate below. The gate only runs on the full resolution path, and a texture that fails it returns before the in-flight claim that is the only thing which erases a want-list entry - so without this test a 256 square texture is added here, waits forever, is fetched to completion by the promotion engine, is refused for being too small, and STAYS on the list. Permanently, and once per sighting.
        //
        // That is not a harmless leak. The list is capped and drops oldest first, so entries which can never be encoded evict entries which can, and the readout says thousands are waiting when most of them are waiting for something that will never happen.
        //
        // The full size is derived rather than passed: a decode at discard d produces the level d dimensions, and the chain halves at every level, so the original is the decoded size shifted back up by d. Exact for every standard mip chain, and the only cost of being wrong would be admitting a texture the gate then refuses once - which is the behaviour we already have.
        bool worth_wanting = (decoded_discard >= 0 && raw.notNull() && !id.isNull());
        if (worth_wanting)
        {
            const U32 shift     = (U32)llclamp(decoded_discard, 0, 16);
            const U64 full_w    = (U64)raw->getWidth()  << shift;
            const U64 full_h    = (U64)raw->getHeight() << shift;
            const U64 min_edge  = (U64)st->mPolicy.mMinSize;
            if (full_w * full_h < min_edge * min_edge) worth_wanting = false;
        }
        if (worth_wanting) noteWanted(st, id);
        // </SS:Nexii>

        return record(st, SSBC7_DECLINE_PARTIAL);
    }

    const U32 width      = raw->getWidth();
    const U32 height     = raw->getHeight();
    const U32 components = (U32)raw->getComponents();
    const U32 min_size   = st->mPolicy.mMinSize;

    // The size gate is on AREA, not on either edge, because area is what the saving is proportional to: a 1024x256 banner holds exactly as many texels as a 512x512, saves exactly as much video memory, and an edge test would throw it away while keeping the square. The setting is still expressed as a square edge length, since "512" and "1024" are how texture sizes are actually spoken about, so the threshold it means is that square's texel count.
    //
    // The separate four texel floor is not about worth, it is about shape: BC7 works in 4x4 blocks, so an edge below four is padded out and the texels paid for are mostly padding. Power of two sources admit 1 and 2, so the case is reachable even though SL content never really produces it.
    const U64 texels     = (U64)width * (U64)height;
    const U64 min_texels = (U64)min_size * (U64)min_size;

    // Non power of two sources cannot be uploaded through the mip path at all.
    if (!isPowerOfTwo(width) || !isPowerOfTwo(height)
        || texels < min_texels
        || width < 4 || height < 4
        || components < 1 || components > 4)
    {
        // <SS:Nexii> Reached full resolution and failed the gate, so it can never be encoded - take it off the want list rather than leave the promotion engine fetching it again on the next sighting. Also the repair path for entries added by a build that predates the check above.
        {
            std::lock_guard<std::mutex> lock(st->mSetMutex);
            st->mWantList.erase(id);
        }
        return record(st, SSBC7_DECLINE_GEOMETRY);
    }

    if (st->mStore->hasRecord(id))
    {
        return record(st, SSBC7_DECLINE_ALREADY);
    }

    const S64 bytes = (S64)raw->getDataSize();
    if (bytes <= 0)
    {
        return record(st, SSBC7_DECLINE_GEOMETRY);
    }

    if (st->mPinnedBytes.load() + bytes > SSBC7_MAX_PINNED_BYTES)
    {
        noteWanted(st, id);
        return record(st, SSBC7_DECLINE_BACKPRESSURE);
    }

    // The in-flight set is what makes plain-uuid dedupe hold across threads: the store only learns about a record once the append completes, so without this two workers can encode the same texture at the same time and the second one's work is thrown away. The verdict is recorded outside the lock, because record() reaches for the same mutex to size the want list.
    bool claimed = false;
    {
        std::lock_guard<std::mutex> lock(st->mSetMutex);
        claimed = st->mInFlight.insert(id).second;
        if (claimed) st->mWantList.erase(id);
    }
    if (!claimed)
    {
        return record(st, SSBC7_DECLINE_ALREADY);
    }

    ++st->mPending;
    st->mPinnedBytes += bytes;

    LLPointer<LLImageRaw> pinned = raw;

    const bool posted = pool->getQueue().tryPost(
        [st, id, pinned, bytes]()
        {
            // Retires the pending count, the pinned bytes and the in-flight entry on THIS thread, whatever happens below - including the abandon path, where nothing else runs at all.
            EncodeGuard guard(st, id, bytes);

            if (st->mAbandon.load()) { ++st->mAbandoned; return; }

            // <SS:Nexii/> Squeeze promotion - the body moved into ssBC7EncodeAndStore so the promotion engine runs exactly this code rather than a second copy of it that would drift.
            //
            // <SS:Nexii/> Squeeze adaptive quality - the profile is read HERE, on the worker, at the moment the encode actually starts, rather than captured when the item was queued. A queue that has been sitting for a minute would otherwise encode at a profile the controller has since abandoned, which is the one case where the queue's own depth is evidence that the older answer was wrong.
            ssBC7EncodeAndStore(id, pinned, ssBC7AdaptiveQualityNow(), false);
        });

    if (!posted)
    {
        // tryPost returns false when the queue is full OR closed, and neither one runs the work item. Without unwinding here the pending count and the pinned-bytes budget leak permanently and the pool refuses everything for the rest of the session.
        --st->mPending;
        st->mPinnedBytes -= bytes;
        {
            std::lock_guard<std::mutex> lock(st->mSetMutex);
            st->mInFlight.erase(id);
        }
        noteWanted(st, id);
        return record(st, SSBC7_DECLINE_POST_FAILED);
    }

    return record(st, SSBC7_ENQUEUED);
}

// <SS:Nexii> Squeeze eviction - the whole main-thread cost of eviction, once a minute: one walk of the resident texture set and at most one work item posted.
//
// It lives here rather than beside the eviction policy for one reason - this is the module that owns the pool, and the trigger inside appendBlob cannot post its own work because it runs under the store lock that the work will itself want. That trigger sets a flag; this reads it, outside every lock, on a thread that is allowed to block for a microsecond.
void ssBC7EncodeMaintenanceTick()
{
    EncodeState* st = state();
    if (!st || st->mShuttingDown || !st->mPolicy.mEnabled) return;

    SSBC7Store* store = st->mStore;
    if (!store || !store->isInitialized() || store->isReadOnly()) return;

    static U32 s_last_second = 0;
    const U32 now = ssBC7NowSeconds();
    if (s_last_second && now >= s_last_second && (now - s_last_second) < SSBC7_EVICT_TICK_SECONDS) return;
    s_last_second = now;

    // THE OTHER HALF OF THE HEAT SIGNAL. hasRecord() only fires when a texture is decoded, so anything that decodes once at login and then stays resident forever - avatar skins, the UI atlas, system assets - would be stamped once and then rank as the coldest thing in the store, which is exactly the set a user would be angriest to lose. Reused across ticks so the walk costs no allocation.
    static std::vector<LLUUID> s_referenced;
    s_referenced.clear();
    for (const LLPointer<LLViewerFetchedTexture>& imagep : gTextureList)
    {
        if (imagep.isNull()) continue;
        const LLUUID& id = imagep->getID();
        if (id.notNull()) s_referenced.push_back(id);
    }

    std::vector<LLUUID> rewant;
    store->touchReferenced(s_referenced, rewant);

    // A texture that is on screen RIGHT NOW and whose record a kill dropped is the only part of a kill worth undoing, and the want list is the existing machinery for saying so. Every other dropped uuid is left alone on purpose: it re-encodes for free the next time it is decoded, on a decode the viewer was going to do anyway.
    for (const LLUUID& id : rewant) noteWanted(st, id);

    if (!store->evictionWanted() && store->allocatedBytes() <= store->budgetBytes()) return;
    if (store->evictionRunning()) return;

    SSBC7EncodePool* pool = st->mPool.load();
    if (!pool) return;

    // tryPost, never post: a refused item is not an error, it is a busy encode queue, and the next tick asks again. No second thread pool - LL::ThreadPool registers by name in an instance tracker, so a second one is more shutdown ordering to get wrong for work that runs once a minute.
    const bool posted = pool->getQueue().tryPost(
        [st, store]()
        {
            if (st->mAbandon.load()) return;
            store->evictPass(true);
        });

    if (!posted)
    {
        LL_DEBUGS("Squeeze") << "BC7 eviction pass not posted: the encode queue is full, retrying at the next tick" << LL_ENDL;
    }
}
// </SS:Nexii>

void ssBC7EncodeWantList(std::vector<LLUUID>& out)
{
    out.clear();
    EncodeState* st = state();
    if (!st) return;

    std::lock_guard<std::mutex> lock(st->mSetMutex);
    st->mWantList.snapshot(out);
}

size_t ssBC7EncodeWantListSize()
{
    EncodeState* st = state();
    if (!st) return 0;

    std::lock_guard<std::mutex> lock(st->mSetMutex);
    return st->mWantList.size();
}

// <SS:Nexii> Squeeze promotion - everything below is the seam ssbc7promote.cpp runs on. None of it is new policy; it is the state this module already owned, made reachable so that promotion shares the one pool, the one queue, the one pinned-bytes budget and the one dedupe set rather than standing up a second of each.

bool ssBC7EncodeGateReady(std::string& out_reason)
{
    EncodeState* st = state();
    if (!st) { out_reason = "the encode module never initialised"; return false; }

    if (!st->mPolicy.mEnabled)          { out_reason = "SSSqueezeEnabled is off"; return false; }
    if (!st->mPolicy.mBackgroundEncode) { out_reason = "SSSqueezeBackgroundEncode is off"; return false; }
    if (!st->mPolicy.mGpuSupported)     { out_reason = "this GPU has no BPTC, so a record would have nothing to serve to"; return false; }
    if (st->mShuttingDown)              { out_reason = "shutdown has begun"; return false; }
    if (!st->mPool.load())              { out_reason = "the encode pool is not running"; return false; }
    if (!st->mStore)                    { out_reason = "the store singleton was never resolved"; return false; }
    if (!st->mStore->isInitialized())   { out_reason = "the store never came up"; return false; }
    if (st->mStore->isReadOnly())       { out_reason = "this is a read-only second instance"; return false; }

    out_reason.clear();
    return true;
}

bool ssBC7EncodeGeometryOK(U32 width, U32 height, U32 components)
{
    EncodeState* st = state();
    if (!st) return false;

    const U32 min_size   = st->mPolicy.mMinSize;
    const U64 texels     = (U64)width * (U64)height;
    const U64 min_texels = (U64)min_size * (U64)min_size;

    // Byte for byte the rule ssBC7EncodeConsider applies, so a texture the demand path would have refused is never given a decode by the promotion path either.
    if (!isPowerOfTwo(width) || !isPowerOfTwo(height)) return false;
    if (texels < min_texels)                           return false;
    if (width < 4 || height < 4)                       return false;
    if (components < 1 || components > 4)              return false;
    return true;
}

S32 ssBC7EncodePendingCount()
{
    EncodeState* st = state();
    return st ? st->mPending.load() : 0;
}

// <SS:Nexii/> Squeeze adaptive quality - the pool's real width, taken from the pool rather than from SSSqueezeEncodeThreads because that setting is zero on every machine that leaves it automatic. Zero here means the pool has not started, and the controller treats that as one worker so its capacity estimate is conservative rather than infinite.
S32 ssBC7EncodePoolWidth()
{
    EncodeState* st = state();
    if (!st) return 0;

    SSBC7EncodePool* pool = st->mPool.load();
    return pool ? (S32)pool->getWidth() : 0;
}

bool ssBC7EncodeAbandonRequested()
{
    EncodeState* st = state();
    return !st || st->mAbandon.load();
}

bool ssBC7EncodeClaim(const LLUUID& id)
{
    EncodeState* st = state();
    if (!st || id.isNull()) return false;

    std::lock_guard<std::mutex> lock(st->mSetMutex);
    const bool claimed = st->mInFlight.insert(id).second;
    if (claimed) st->mWantList.erase(id);
    return claimed;
}

void ssBC7EncodeUnclaim(const LLUUID& id)
{
    EncodeState* st = state();
    if (!st) return;

    std::lock_guard<std::mutex> lock(st->mSetMutex);
    st->mInFlight.erase(id);
}

bool ssBC7EncodeReserveBytes(S64 bytes)
{
    EncodeState* st = state();
    if (!st || bytes <= 0) return false;

    // Not a compare-exchange loop on purpose. The only two writers are this call and the demand path, both of which over-reserve rather than under-reserve on a race, and a ceiling that is briefly a few megabytes conservative costs one deferred encode - while a loop here would be a lock-free retry in code whose whole job is to be uninteresting.
    if (st->mPinnedBytes.load() + bytes > SSBC7_MAX_PINNED_BYTES) return false;

    // mPending is deliberately NOT touched. It counts DEMAND path encodes and is what the promotion engine reads to decide the pool is idle, so counting promotion work in it would make the engine permanently believe it had arrived at a busy moment.
    st->mPinnedBytes += bytes;
    return true;
}

void ssBC7EncodeReleaseBytes(S64 bytes)
{
    EncodeState* st = state();
    if (!st || bytes <= 0) return;

    st->mPinnedBytes -= bytes;
}

bool ssBC7EncodeTryPost(const std::function<void()>& work)
{
    EncodeState* st = state();
    if (!st || st->mShuttingDown) return false;

    SSBC7EncodePool* pool = st->mPool.load();
    if (!pool) return false;

    // tryPost, always. WorkQueue::post BLOCKS when the queue is full, and the caller here is the frame thread - a blocked frame thread is a frozen viewer, for work whose entire justification is that nobody is waiting on it.
    return pool->getQueue().tryPost(work);
}

bool ssBC7EncodeAndStore(const LLUUID& id, const LLPointer<LLImageRaw>& raw, SSBC7Quality quality, bool allow_supersede)
{
    EncodeState* st = state();
    if (!st || raw.isNull()) return false;
    if (!st->mStore || !st->mStore->isInitialized() || st->mStore->isReadOnly()) return false;

    // <SS:Nexii> Squeeze adaptive quality - the occupancy signal, bracketed around the WHOLE of this call rather than around the block loop alone, because from the overlay's point of view a worker holding a decoded image and waiting on the store lock is just as busy as one inside the vector units. Scoped so every early return below retires it, which is the same lesson EncodeGuard exists for.
    struct BusyMark
    {
        BusyMark()  { ssBC7AdaptiveNoteBusy(true); }
        ~BusyMark() { ssBC7AdaptiveNoteBusy(false); }
    } busy_mark;
    // </SS:Nexii>

    // One scratch per worker thread, reused for every texture that thread ever encodes, so a busy pool does no allocation per image and none whatsoever per block.
    static thread_local SSBC7EncodeScratch scratch;

    std::vector<U8> payload;
    std::vector<U8> pickmask;   // <SS:Nexii/> Squeeze pick masks - empty for an opaque texture
    SSBC7EncodeResult result;
    U16 flags = 0;
    U32 width = 0;
    U32 height = 0;
    U32 components = 0;

    {
        // The raw is treated as strictly immutable here. The shared lock is what makes that a promise rather than a hope, since the decode pool and the cache writer both reach for the same object.
        LLImageDataSharedLock lock(raw.get());

        const U8* src = raw->getData();
        width      = raw->getWidth();
        height     = raw->getHeight();
        components = (U32)raw->getComponents();

        if (!src || raw->isBufferInvalid())
        {
            ++st->mEncodeFailed;
            LL_WARNS("Squeeze") << "BC7 encode of " << id << " skipped: the decoded image was released before the worker reached it" << LL_ENDL;
            return false;
        }

        classifyAlpha(src, width, height, components, flags);

        // <SS:Nexii/> Squeeze pick masks - only a texture with something transparent gets one; a fully opaque texture never had a mask under the stock path either and getMask answers true without one. Built inside the shared lock because it reads the raw, and outside the encode timing because it is not the encoder's cost.
        if ((flags & SSBC7_FLAG_FULLY_OPAQUE) == 0)
        {
            buildPickMask(src, width, height, components, pickmask);
        }

        // <SS:Nexii/> Squeeze adaptive quality - the encode is timed and NOTHING ELSE IS. The decode, the J2C read, the alpha classification and the store append are all excluded deliberately: the number the controller acts on has to be the cost of the profile it is choosing between, and folding in work that is identical at every profile would flatten the ladder and make a step down look as though it bought almost nothing.
        const std::chrono::steady_clock::time_point encode_began = std::chrono::steady_clock::now();

        if (!ssBC7EncodeMipChain(src, width, height, components, quality, scratch, payload, result))
        {
            ++st->mEncodeFailed;
            LL_WARNS("Squeeze") << "BC7 encode of " << id << " failed at " << width << "x" << height
                                << " with " << components << " components, the texture keeps using the ordinary J2C path" << LL_ENDL;
            return false;
        }

        // <SS:Nexii/> Squeeze adaptive quality - reported only for an encode that SUCCEEDED, because a failure's duration measures how long it took to give up rather than how fast the machine is, and a run of failures would otherwise read as a suddenly very fast profile.
        const F64 encode_seconds = std::chrono::duration<F64>(std::chrono::steady_clock::now() - encode_began).count();
        ssBC7AdaptiveNoteEncode((SSBC7Quality)result.mQuality, result.mEncodedTexels, encode_seconds);
    }

    if (st->mAbandon.load()) { ++st->mAbandoned; return false; }

    SSBC7Encoded enc;
    enc.mUUID          = id;
    enc.mWidth         = (U16)result.mWidth;
    enc.mHeight        = (U16)result.mHeight;
    enc.mMipCount      = result.mMipCount;
    enc.mSrcComponents = result.mSrcComponents;   // the ORIGINAL component count, which is what later keeps an opaque texture out of the alpha pool
    enc.mQuality       = result.mQuality;         // <SS:Nexii/> Squeeze adaptive quality - taken from the RESULT rather than from the argument, so the record says what the encoder actually did even if a future backend clamps a profile it cannot honour
    enc.mFlags         = flags;
    enc.mPayload       = std::move(payload);
    enc.mPickMask      = std::move(pickmask);   // <SS:Nexii/> Squeeze pick masks - ssBC7BuildBlob sets SSBC7_FLAG_HAS_PICKMASK from this being non-empty

    // The append is done here rather than posted back to the main thread for the same reason the accounting is: a completion callback does not run once the main loop stops, and a store write is disk work that has no business on the frame thread anyway.
    if (!st->mStore->append(enc, ssBC7EncoderVersion(), allow_supersede))
    {
        LL_DEBUGS("Squeeze") << "BC7 store declined " << id << ", see the store metrics for whether that was a duplicate or a write failure" << LL_ENDL;
        return false;
    }

    // A texture the read path already probed and declined for having no record will never look again on its own, so the uuid is handed to the main thread to re-arm. The list is capped for the same reason everything else here is: if nothing is draining it, it must not grow without limit.
    {
        std::lock_guard<std::mutex> lock(st->mSetMutex);
        if (st->mFreshlyStored.size() < 4096) st->mFreshlyStored.push_back(id);
    }
    return true;
}

size_t ssBC7EncodeTakeWanted(size_t max_count, std::vector<LLUUID>& out)
{
    out.clear();
    EncodeState* st = state();
    if (!st) return 0;

    std::lock_guard<std::mutex> lock(st->mSetMutex);
    return st->mWantList.take(max_count, out);
}

void ssBC7EncodeWant(const LLUUID& id)
{
    EncodeState* st = state();
    if (!st || id.isNull()) return;
    noteWanted(st, id);
}

U32 ssBC7EncodeWantDroppedTotal()
{
    EncodeState* st = state();
    if (!st) return 0;

    std::lock_guard<std::mutex> lock(st->mSetMutex);
    return st->mWantList.droppedTotal();
}

void ssBC7EncodeTakeFreshlyStored(std::vector<LLUUID>& out)
{
    out.clear();
    EncodeState* st = state();
    if (!st) return;

    std::lock_guard<std::mutex> lock(st->mSetMutex);
    out.swap(st->mFreshlyStored);
}
// </SS:Nexii>

std::string ssBC7EncodeMetricsString()
{
    EncodeState* st = state();
    if (!st) return "BC7 encode pool not started";

    std::string counts;
    for (U32 i = 0; i < SSBC7_VERDICT_COUNT; ++i)
    {
        const U32 n = st->mVerdicts[i].load();
        if (!n) continue;
        if (!counts.empty()) counts += ", ";
        counts += llformat("%s %u", ssBC7VerdictName((ESSBC7EncodeVerdict)i), n);
    }
    if (counts.empty()) counts = "nothing considered yet";

    return llformat("BC7 encode: %s | in flight %d, pinned %lld MB, abandoned %u, encode failures %u, want list %u",
                    counts.c_str(),
                    (S32)st->mPending.load(),
                    (long long)(st->mPinnedBytes.load() / (1024 * 1024)),
                    st->mAbandoned.load(),
                    st->mEncodeFailed.load(),
                    (U32)ssBC7EncodeWantListSize());
}
