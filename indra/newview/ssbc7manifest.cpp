/**
 * @file ssbc7manifest.cpp
 * @brief Squeeze region texture manifests - records what mattered in a region and pre-warms its BC7 off the local disk on returning, see doc/viewer/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssbc7manifest.h"

#include "llagent.h"
#include "llappviewer.h"
#include "lldir.h"
#include "llfile.h"
#include "llstartup.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewernetwork.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "ssbc7encodequeue.h"
#include "ssbc7serve.h"
#include "ssbc7store.h"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <mutex>

// WHERE EVERY PIECE OF THIS RUNS, because the split is forced rather than chosen.
//
// The TICK, the RECORDER and the PRE-WARM are main thread: gAgent, gTextureList, LLViewerTextureManager and the read path's request stage are all main-thread affairs, and the recorder in particular must be on the thread that already owns the texture list rather than on a fetch thread, because a fetch thread taking a per-texture lock to file a uuid would put this feature's cost squarely on the path it exists to shorten.
//
// The LOAD and the SAVE are the only file IO, and both go on the existing BC7Encode pool through ssBC7EncodeTryPost. Not a new pool: a third one would be more shutdown ordering to get wrong for work nobody is waiting on. Not the reader pool either, because that one exists to gate visible textures and an 80 KB manifest write has no business queueing ahead of one. Background QoS is exactly right for a write nobody waits on, and for a read that has whole seconds of teleport screen to complete in.
//
// NOTHING HERE EVER ORIGINATES SIMULATOR OR ASSET TRAFFIC. The pre-warm only ever touches a uuid the store already has a record for; a manifest entry with no record is counted and dropped, and is a want-list candidate at most. That is the difference between a warm-up and a speculative download.

namespace
{
    // The pre-warm serves a REDUCED prefix rather than the full chain, and this is the single most important number in the file. A full-res warm of a dense region would be a gigabyte of video memory spent on textures nobody has looked at yet; discard 2 is a sixteenth of that, is already sharper than anything the network will have delivered in the same window, and the ordinary RESIDENT-to-READING edge in updateFetch upgrades each texture to its real wanted level the moment an object actually references it. Warming cheap and upgrading on demand beats warming expensively and evicting.
    const U32 SSBC7_MANIFEST_DEFAULT_DISCARD = 2;
    const U32 SSBC7_MANIFEST_DEFAULT_MB      = 128;

    const F64 SSBC7_MANIFEST_LOG_SECONDS     = 120.0;

    // One pre-warm that has been posted and not yet confirmed to have reached video memory. The bytes are carried rather than looked up again because the record can be evicted between the post and the check, and a metric that silently forgets what it spent is worse than no metric.
    struct PendingWarm
    {
        LLUUID mUUID;
        U32    mBC7Bytes   = 0;
        U32    mSavedBytes = 0;
        F64    mIssued     = 0.0;
    };

    struct ManifestState
    {
        std::atomic<bool>   mEnabled{false};
        std::atomic<bool>   mShuttingDown{false};
        SSBC7Store*         mStore{nullptr};      // resolved on the main thread at init, because a worker must never be the first toucher of an LLSingleton

        std::string         mDir;                 // <cache>/texturecache/bc7cache/manifests
        std::string         mGridId;

        U32                 mWarmDiscard{SSBC7_MANIFEST_DEFAULT_DISCARD};
        U64                 mWarmBudgetBytes{(U64)SSBC7_MANIFEST_DEFAULT_MB << 20};

        // ---- the region currently being recorded ----
        U64                 mHandle{0};
        U32                 mVisits{0};
        SSBC7ManifestSet    mLive{SSBC7_MANIFEST_MAX_ENTRIES};

        // ---- the pre-warm episode for the region just arrived in ----
        std::vector<SSBC7ManifestEntry> mWarmQueue;
        size_t                          mWarmCursor{0};
        U64                             mWarmReserved{0};   // bytes claimed by posts not yet confirmed or written off, which is what the budget is actually enforced against
        U32                             mWarmAttempts{0};
        std::vector<PendingWarm>        mPending;
        size_t                          mVerifyCursor{0};

        // ---- the async load, handed back from the pool ----
        std::mutex                      mLoadMutex;
        bool                            mLoadReady{false};
        std::atomic<bool>               mLoadInFlight{false};
        U64                             mLoadHandle{0};
        SSBC7ManifestHeader             mLoadHeader;
        std::vector<SSBC7ManifestEntry> mLoadEntries;
        ESSBC7ManifestVerdict           mLoadVerdict{SSBC7_MANIFEST_DECLINE_NO_FILE};

        std::atomic<U32>    mVerdicts[SSBC7_MANIFEST_VERDICT_COUNT];
        SSBC7ManifestStats  mStats;               // main thread only, except the two counters below

        std::atomic<U32>    mSaved{0};
        std::atomic<U32>    mSaveFailed{0};

        F64                 mNextTick{0.0};
        F64                 mNextRecord{0.0};
        F64                 mNextLog{0.0};
        bool                mPrunedThisSession{false};

        ManifestState()
        {
            for (U32 i = 0; i < SSBC7_MANIFEST_VERDICT_COUNT; ++i) mVerdicts[i] = 0;
        }
    };

    // Created once on the main thread and then never destroyed, exactly as the encode and serve states are: a pool worker that loads this pointer while the viewer is quitting has to find a shutting-down state rather than freed memory.
    std::atomic<ManifestState*> s_state{nullptr};

    ManifestState* state() { return s_state.load(std::memory_order_acquire); }

    void record(ManifestState* st, ESSBC7ManifestVerdict verdict)
    {
        ++st->mVerdicts[verdict];
        st->mStats.mLastVerdict = (U32)verdict;
    }

    std::string manifestPath(ManifestState* st, U64 handle)
    {
        if (st->mDir.empty() || !handle) return std::string();
        return gDirUtilp->add(st->mDir, ssBC7ManifestFileName(st->mGridId, handle));
    }

    // A texture is worth remembering for this region only if it is the kind of thing the read path could ever serve. Testing that here rather than at warm time is what keeps a manifest from filling up with sculpt maps and server bakes that would be looked up and declined once per visit forever.
    bool worthRecording(LLViewerFetchedTexture* tex)
    {
        if (!tex) return false;
        if (tex->getID().isNull()) return false;

        // Keyed on the object's own type for the same reason ssbc7serve.cpp keys its exclusions that way: records are keyed by plain uuid while texture objects are keyed by (uuid, ETexListType), so the same uuid is reachable through a differently typed object.
        const S8 type = tex->getType();
        if (type != LLViewerTexture::FETCHED_TEXTURE && type != LLViewerTexture::LOD_TEXTURE) return false;
        if (tex->getFTType() != FTT_DEFAULT) return false;

        // Only the standard key is recorded, because that is the only key the pre-warm can recreate a texture under. A uuid remembered under a scaled or sculpt key would be warmed into an object nothing looks at.
        if (tex->getTextureListType() != TEX_LIST_STANDARD) return false;

        if (tex->forSculpt() || tex->getBoostLevel() == LLGLTexture::BOOST_SCULPTED) return false;
        if (tex->getBoostLevel() == LLGLTexture::BOOST_UI) return false;
        if (tex->getBoostLevel() == LLGLTexture::BOOST_ICON) return false;
        if (tex->getBoostLevel() == LLGLTexture::BOOST_THUMBNAIL) return false;
        if (tex->ssBC7HadExplicitFormat()) return false;

        return true;
    }

    // ONE WALK OF THE TEXTURE LIST, on the same thread and in the same shape as the eviction pass's heat sweep, and at a quarter of its rate. The signal is mMaxVirtualSize, which the texture list has already computed this cycle - a texture with a non-zero one is a texture something on screen wanted, which is exactly the definition of "mattered here". Reading it costs a load; computing anything better would cost a scene traversal this feature does not deserve.
    //
    // The set is not restricted to objects owned by the current region, deliberately: a neighbour's build is on this screen and its textures are worth having in place next time the user stands in this spot. The manifest describes a VIEWPOINT, not a parcel.
    void recordSweep(ManifestState* st)
    {
        if (!st->mHandle) return;

        U32 seen = 0;
        for (const LLPointer<LLViewerFetchedTexture>& imagep : gTextureList)
        {
            LLViewerFetchedTexture* tex = imagep.get();
            if (!tex) continue;

            const F32 vsize = tex->getMaxVirtualSize();
            if (vsize <= 0.f) continue;
            if (!worthRecording(tex)) continue;

            // Clamped rather than scaled, because the weight is only ever compared against other weights and a texel count past four billion means the same thing as four billion.
            const U32 weight = (U32)llclamp(vsize, 1.f, 4.0e9f);
            st->mLive.note(tex->getID(), weight);
            ++seen;
        }

        st->mStats.mRecorded      = (U32)st->mLive.size();
        st->mStats.mRecordDropped = st->mLive.dropped();

        LL_DEBUGS("Squeeze") << "BC7 manifest recorder saw " << seen << " wanted textures, region set now " << st->mLive.size()
                             << " of " << st->mLive.capacity() << ", dropped " << st->mLive.dropped() << " so far this session" << LL_ENDL;
    }

    // Hands the current region's set to the pool to be written. The SORT and the SERIALISATION go with it rather than happening here, because region change is the hot moment this project has already shipped one stall into; what the main thread pays is one copy of at most 4096 uuids out of a hash map.
    void postSave(ManifestState* st)
    {
        if (!st->mHandle) return;

        const std::string path = manifestPath(st, st->mHandle);
        if (path.empty()) return;

        SSBC7ManifestHeader hdr;
        hdr.mRegionHandle = st->mHandle;
        hdr.mLastSeen     = (U64)time(nullptr);
        hdr.mDropped      = st->mLive.dropped();
        hdr.mVisits       = st->mVisits;

        std::vector<SSBC7ManifestEntry> entries;
        st->mLive.take(entries);

        if (entries.empty())
        {
            // Nothing was ever wanted here, which happens on a region crossed through in two seconds. Writing an empty manifest would only cost a later visit a read that tells it nothing, and would push a real manifest out of the directory cap.
            LL_DEBUGS("Squeeze") << "BC7 manifest for handle " << st->mHandle << " not written: nothing was recorded" << LL_ENDL;
            return;
        }

        const U32 count = (U32)entries.size();
        const U64 handle = st->mHandle;

        const bool posted = ssBC7EncodeTryPost(
            [st, path, hdr, entries, count, handle]()
            {
                // Deliberately NOT gated on ssBC7EncodeAbandonRequested(). The abandon flag exists so that a quit does not wait on a backlog of multi-hundred-millisecond encodes, and this is one small file write that the last region of the session depends on: the shutdown sequence posts this BEFORE the pool is told to abandon, and honouring the flag anyway would lose the manifest to a race with whichever worker happened to pick the item up. This module's own flag is the correct gate, and it is only raised after the pool has been joined.
                if (st->mShuttingDown.load()) return;

                const ESSBC7ManifestVerdict verdict = ssBC7ManifestWrite(path, hdr, entries);
                if (verdict == SSBC7_MANIFEST_OK)
                {
                    ++st->mSaved;
                    LL_INFOS("Squeeze") << "BC7 manifest saved for handle " << handle << ": " << count << " textures, "
                                        << hdr.mDropped << " dropped by the cap over its life" << LL_ENDL;
                }
                else
                {
                    ++st->mSaveFailed;
                    LL_WARNS("Squeeze") << "BC7 manifest NOT saved for handle " << handle << ": " << ssBC7ManifestVerdictName(verdict)
                                        << " at " << path << " - the previous manifest is untouched" << LL_ENDL;
                }
            });

        if (!posted)
        {
            // A refused post is a busy encode queue and never an error. The cost is one region's worth of learning, and the set is about to be cleared anyway, so there is nothing to retry with.
            LL_WARNS("Squeeze") << "BC7 manifest for handle " << handle << " not written: the encode queue refused the post" << LL_ENDL;
        }
    }

    void postLoad(ManifestState* st, U64 handle)
    {
        const std::string path = manifestPath(st, handle);
        if (path.empty()) return;

        // The once-a-session directory prune rides along with the first load rather than sitting in init, because it is a directory walk plus one small read per file and init is already the busiest moment the viewer has.
        const bool prune = !st->mPrunedThisSession;
        st->mPrunedThisSession = true;

        const std::string dir = st->mDir;

        st->mLoadInFlight = true;

        const bool posted = ssBC7EncodeTryPost(
            [st, path, dir, handle, prune]()
            {
                if (st->mShuttingDown.load() || ssBC7EncodeAbandonRequested())
                {
                    st->mLoadInFlight = false;
                    return;
                }

                SSBC7ManifestHeader hdr;
                std::vector<SSBC7ManifestEntry> entries;
                const ESSBC7ManifestVerdict verdict = ssBC7ManifestRead(path, handle, hdr, entries);

                {
                    std::lock_guard<std::mutex> lock(st->mLoadMutex);
                    st->mLoadHandle  = handle;
                    st->mLoadHeader  = hdr;
                    st->mLoadEntries = std::move(entries);
                    st->mLoadVerdict = verdict;
                    st->mLoadReady   = true;
                }

                if (prune)
                {
                    const U32 removed = ssBC7ManifestPruneDir(dir, SSBC7_MANIFEST_MAX_FILES);
                    if (removed) LL_INFOS("Squeeze") << "BC7 manifest directory pruned: " << removed << " removed, keeping the " << SSBC7_MANIFEST_MAX_FILES << " most recently seen regions" << LL_ENDL;
                }

                st->mLoadInFlight = false;
            });

        if (!posted)
        {
            // Left un-loaded rather than retried, because the encode queue being full at region arrival means the demand path is busy, and that is precisely the moment this feature is supposed to stay out of the way.
            st->mLoadInFlight = false;
            LL_WARNS("Squeeze") << "BC7 manifest for handle " << handle << " not loaded: the encode queue refused the post" << LL_ENDL;
        }
    }

    void beginRegion(ManifestState* st, U64 handle)
    {
        // Everything the previous region had in flight is abandoned rather than carried over: its warm queue names textures that are no longer on this screen, and its pending list would otherwise attribute the new region's residency to the old one's pre-warm.
        st->mWarmQueue.clear();
        st->mWarmCursor   = 0;
        st->mWarmReserved = 0;
        st->mWarmAttempts = 0;
        st->mPending.clear();
        st->mVerifyCursor = 0;

        {
            std::lock_guard<std::mutex> lock(st->mLoadMutex);
            st->mLoadReady = false;
            st->mLoadEntries.clear();
        }

        st->mHandle = handle;
        st->mVisits = 1;
        st->mLive.clear();

        if (handle) postLoad(st, handle);
    }

    void drainLoad(ManifestState* st)
    {
        SSBC7ManifestHeader hdr;
        std::vector<SSBC7ManifestEntry> entries;
        ESSBC7ManifestVerdict verdict = SSBC7_MANIFEST_OK;
        U64 handle = 0;

        {
            std::lock_guard<std::mutex> lock(st->mLoadMutex);
            if (!st->mLoadReady) return;
            st->mLoadReady = false;
            hdr     = st->mLoadHeader;
            entries = std::move(st->mLoadEntries);
            verdict = st->mLoadVerdict;
            handle  = st->mLoadHandle;
            st->mLoadEntries.clear();
        }

        record(st, verdict);

        // The agent left again while the read was out. Applying it now would warm the region behind us.
        if (handle != st->mHandle) return;

        if (verdict != SSBC7_MANIFEST_OK)
        {
            if (verdict != SSBC7_MANIFEST_DECLINE_NO_FILE)
            {
                ++st->mStats.mManifestsRefused;
                LL_WARNS("Squeeze") << "BC7 manifest for handle " << handle << " refused: " << ssBC7ManifestVerdictName(verdict)
                                    << " - this region will be re-learned from scratch and rewritten on exit" << LL_ENDL;
            }
            return;
        }

        ++st->mStats.mManifestsLoaded;
        st->mStats.mEntriesLoaded += (U32)entries.size();
        st->mVisits = hdr.mVisits + 1;

        // Folded into what this visit sees so the manifest written on exit is the UNION rather than only what happened to be on screen during a thirty second stop. Halved on the way in so a set built over many visits keeps re-earning its place instead of ossifying around whatever the first visit looked at.
        st->mLive.mergeDecayed(entries);

        // The warm queue is the file's own order, which the writer already sorted heaviest first, so the biggest contributors to video memory land first and a budget that runs out runs out on the least important entries.
        st->mWarmQueue = std::move(entries);
        st->mWarmCursor = 0;

        LL_INFOS("Squeeze") << "BC7 manifest loaded for handle " << handle << ": " << st->mWarmQueue.size() << " textures, visit "
                            << st->mVisits << ", " << hdr.mDropped << " uuids the cap dropped on previous visits (so this list is a floor, not a total)" << LL_ENDL;
    }

    // How many store-order levels serving `discard` needs, mirroring levelsForDiscard in ssbc7serve.cpp. Duplicated rather than exported because it is three lines of arithmetic and exporting it would be a second edit to a file another workstream is live in.
    U32 levelsForDiscard(U32 mip_count, S32 discard)
    {
        if (!mip_count) return 0;
        const S32 d = llclamp(discard, 0, (S32)mip_count - 1);
        return mip_count - (U32)d;
    }

    void warmPass(ManifestState* st, F64 now)
    {
        if (st->mWarmCursor >= st->mWarmQueue.size()) return;
        if (!ssBC7ServeEnabled()) return;
        if (!st->mStore || !st->mStore->isInitialized()) return;

        if (st->mWarmAttempts >= SSBC7_MANIFEST_WARM_MAX) return;
        if (st->mWarmReserved >= st->mWarmBudgetBytes) return;

        // FOREGROUND FETCH ALWAYS WINS, and this is the whole of that promise. The reader pool is shared with the demand path, so a pass only ever adds to it when the pool has drained - which in a steady frame it has, since a prefix read is a fraction of a millisecond. A pass that finds work in flight simply does nothing and asks again a quarter of a second later, which is exactly the right behaviour when the user is looking at something.
        if (ssBC7ServeReadsInFlight() > 0) return;

        // TWO BOUNDS, because they stop different things. `issued` bounds what is added to the shared reader queue; `examined` bounds the MAIN THREAD, because on a cold store every entry costs a store lookup and returns no-record, and without this a manifest of two thousand uuids none of which are encoded yet would be walked end to end inside one frame.
        U32 issued = 0;
        U32 examined = 0;
        while (issued < SSBC7_MANIFEST_WARM_PER_TICK
               && examined < SSBC7_MANIFEST_WARM_PER_TICK * 4
               && st->mWarmCursor < st->mWarmQueue.size())
        {
            const LLUUID id = st->mWarmQueue[st->mWarmCursor++].mUUID;
            ++st->mWarmAttempts;
            ++examined;

            // ONLY EVER A TEXTURE THE STORE ALREADY HAS. A manifest entry with no record is counted and dropped here and goes no further: turning it into a fetch would make a warm-up into a speculative download of a region the user may be leaving in ten seconds, which is not what this path is for and not something it is allowed to do.
            SSBC7Record rec;
            if (!st->mStore->lookup(id, rec))
            {
                ++st->mStats.mWarmNoRecord;
                continue;
            }

            const U32 levels = levelsForDiscard(rec.mMipCount, (S32)st->mWarmDiscard);
            const U32 bc7_bytes = ssBC7PrefixBytes(rec.mWidth, rec.mHeight, rec.mMipCount, levels);
            if (!bc7_bytes)
            {
                ++st->mStats.mWarmDeclined;
                continue;
            }

            if (st->mWarmReserved + bc7_bytes > st->mWarmBudgetBytes)
            {
                // The budget is spent. Stopping rather than skipping to something smaller is deliberate: the queue is in descending weight order, so everything past here matters less than what has already been placed.
                st->mWarmCursor = st->mWarmQueue.size();
                LL_INFOS("Squeeze") << "BC7 manifest pre-warm stopped at its video memory budget of " << (st->mWarmBudgetBytes >> 20)
                                    << " MB after " << st->mStats.mWarmIssued << " textures" << LL_ENDL;
                break;
            }

            LLViewerFetchedTexture* tex = gTextureList.findImage(id, TEX_LIST_STANDARD);
            if (tex && tex->ssBC7IsResident())
            {
                ++st->mStats.mWarmAlreadyResident;
                continue;
            }

            if (!tex)
            {
                // Creating the texture object is what makes this a pre-warm rather than a read into nowhere, and it is safe precisely because an unreferenced texture is inert: decode_priority is mMaxVirtualSize, which is zero until something on screen wants it, and updateFetch refuses to make a J2C request at zero. So this can never turn into network traffic, and if nothing ever references it the ordinary lazy flush in updateImageDecodePriority deletes it thirty seconds later and hands the video memory straight back.
                tex = LLViewerTextureManager::getFetchedTexture(id, FTT_DEFAULT, true, LLGLTexture::BOOST_NONE, LLViewerTexture::LOD_TEXTURE);
            }

            if (!tex)
            {
                ++st->mStats.mWarmDeclined;
                continue;
            }

            // createImage already ran the probe, so anything the read path will not serve is sitting at DECLINED with its reason recorded there. Re-deriving that judgement here would be a second copy of the exclusion list to keep in step.
            if (tex->ssBC7Residency() != (U8)LLViewerFetchedTexture::SSBC7_RES_HIT_KNOWN)
            {
                ++st->mStats.mWarmDeclined;
                continue;
            }

            const ESSBC7ServeVerdict verdict = ssBC7ServeRequest(tex, (S32)st->mWarmDiscard);
            if (verdict != SSBC7_SERVE_QUEUED)
            {
                // Almost always a busy reader queue, which is not an error and not this pass's business to retry - the entry is simply past.
                ++st->mStats.mWarmRefused;
                continue;
            }

            // BC7 is a flat one byte per texel and the source was mSrcComponents bytes per texel, so the saving is the ratio less one. Derived rather than measured because the only alternative would be to decode the J2C to find out, which is the work this whole tier exists to avoid.
            const U32 saved = rec.mSrcComponents > 1 ? bc7_bytes * (U32)(rec.mSrcComponents - 1) : 0;

            st->mWarmReserved += bc7_bytes;
            ++st->mStats.mWarmIssued;
            st->mPending.push_back(PendingWarm{id, bc7_bytes, saved, now});
            ++issued;
        }
    }

    // Confirms which pre-warms actually reached video memory, which is the only honest way to answer "how much did this put in place". A posted read is not a resident texture: the blob can fail to verify, the upload can be refused, and an exclusion can latch between the two.
    void verifyPass(ManifestState* st, F64 now)
    {
        if (st->mPending.empty()) return;

        if (st->mVerifyCursor >= st->mPending.size()) st->mVerifyCursor = 0;

        U32 looked = 0;
        while (looked < SSBC7_MANIFEST_VERIFY_PER_TICK && st->mVerifyCursor < st->mPending.size())
        {
            const PendingWarm& p = st->mPending[st->mVerifyCursor];
            ++looked;

            LLViewerFetchedTexture* tex = gTextureList.findImage(p.mUUID, TEX_LIST_STANDARD);

            bool settled = false;
            if (tex && tex->ssBC7IsResident())
            {
                ++st->mStats.mWarmLanded;
                st->mStats.mWarmBytesInPlace += p.mBC7Bytes;
                st->mStats.mWarmBytesSaved   += p.mSavedBytes;
                settled = true;
            }
            else if (!tex || tex->ssBC7Residency() == (U8)LLViewerFetchedTexture::SSBC7_RES_DECLINED)
            {
                // The texture is gone or the read path gave up on it. The reservation comes back so a session of failures cannot silently exhaust the budget and make the pre-warm look like it simply stopped.
                if (st->mWarmReserved >= p.mBC7Bytes) st->mWarmReserved -= p.mBC7Bytes;
                ++st->mStats.mWarmDeclined;
                settled = true;
            }
            else if (now - p.mIssued > 30.0)
            {
                // Still READING after half a minute means the completion was lost or the texture was re-requested by the ordinary path. Either way this entry will never be attributed, and holding its bytes forever would be the quiet leak version of the same bug.
                if (st->mWarmReserved >= p.mBC7Bytes) st->mWarmReserved -= p.mBC7Bytes;
                settled = true;
            }

            if (settled)
            {
                st->mPending[st->mVerifyCursor] = st->mPending.back();
                st->mPending.pop_back();
            }
            else
            {
                ++st->mVerifyCursor;
            }
        }
    }

    void maybeLog(ManifestState* st, F64 now)
    {
        if (now < st->mNextLog) return;
        st->mNextLog = now + SSBC7_MANIFEST_LOG_SECONDS;

        // Only once something has happened, or a quiet session becomes a wall of identical lines.
        if (!st->mStats.mManifestsLoaded && !st->mStats.mManifestsRefused && !st->mStats.mWarmIssued && !st->mStats.mRecorded) return;

        LL_INFOS("Squeeze") << ssBC7ManifestMetricsString() << LL_ENDL;
    }
}

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

void ssBC7ManifestInit()
{
    if (state()) return;

    ManifestState* st = new ManifestState();
    st->mStore = &SSBC7Store::instance();
    s_state.store(st, std::memory_order_release);

    ssBC7ManifestRefreshPolicy();
}

void ssBC7ManifestRefreshPolicy()
{
    ManifestState* st = state();
    if (!st) return;

    const bool enabled = gSavedSettings.getBOOL("SSSqueezeEnabled") && gSavedSettings.getBOOL("SSSqueezeManifests");

    // Clamped rather than trusted: a discard past the chain would serve nothing at all, and a zero budget would read as "unlimited" to arithmetic that only ever compares against it.
    st->mWarmDiscard      = (U32)llclamp((S32)gSavedSettings.getU32("SSSqueezeManifestWarmDiscard"), 0, 5);
    const U32 budget_mb   = (U32)llclamp((S32)gSavedSettings.getU32("SSSqueezeManifestWarmMB"), 0, 4096);
    st->mWarmBudgetBytes  = (U64)budget_mb << 20;

    // The directory is resolved from the store rather than rebuilt from gDirUtilp, so it can only ever be inside bc7cache - which is what makes the recursive purge in SSBC7Store::purgeAll take the manifests along and satisfy the "clear cache must erase where you went" requirement without a second hook to forget.
    if (st->mStore && st->mStore->isInitialized())
    {
        const std::string dir = gDirUtilp->add(st->mStore->storeDir(), SSBC7_MANIFEST_DIR_NAME);
        if (dir != st->mDir)
        {
            st->mDir = dir;
            LLFile::mkdir(st->mDir);
        }
    }

    st->mGridId = LLGridManager::getInstance()->getGridId();
    if (st->mGridId.empty()) st->mGridId = "unknown";

    const bool was = st->mEnabled.exchange(enabled);
    if (was != enabled)
    {
        LL_INFOS("Squeeze") << "BC7 region manifests are now " << (enabled ? "on" : "off")
                            << ", pre-warm at discard " << st->mWarmDiscard << " within " << (st->mWarmBudgetBytes >> 20) << " MB, at " << st->mDir << LL_ENDL;
    }
}

void ssBC7ManifestTick()
{
    ManifestState* st = state();
    if (!st || st->mShuttingDown.load()) return;
    if (!st->mEnabled.load()) return;

    // Nothing here reads gAgent until there is a session for it to describe, in the same order the promotion engine checks it.
    if (gDisconnected || LLStartUp::getStartupState() != STATE_STARTED) return;
    if (!st->mStore || !st->mStore->isInitialized()) return;

    if (st->mDir.empty())
    {
        // The store came up after this module did, which is the ordinary case on a first run - the directory is resolved once and this branch is never taken again.
        ssBC7ManifestRefreshPolicy();
        if (st->mDir.empty()) return;
    }

    // THE REGION CHANGE DETECTOR, and it is the entire hook. One pointer dereference and one 64 bit comparison per frame, no listener, no new call site in llviewerregion.cpp, and nothing for another workstream editing that file to collide with. A null region is ignored rather than treated as a departure, because a teleport passes through one and saving the outgoing manifest twice would be the only effect.
    LLViewerRegion* region = gAgent.getRegion();
    const U64 handle = region ? region->getHandle() : 0;
    if (handle && handle != st->mHandle)
    {
        // Re-read HERE rather than trusting the value cached at init, because init runs before login and the grid is not known until then - a manifest filed under the startup default would be looked up under the real grid on the next visit and never found again.
        std::string grid = LLGridManager::getInstance()->getGridId();
        if (grid.empty()) grid = "unknown";
        if (grid != st->mGridId)
        {
            // The outgoing region belongs to the grid it was recorded on, so the save below still uses the old id and the change only takes effect for the region being entered.
            if (st->mHandle) postSave(st);
            st->mGridId = grid;
            st->mHandle = 0;
        }

        if (st->mHandle) postSave(st);
        beginRegion(st, handle);
    }

    const F64 now = LLTimer::getElapsedSeconds();
    if (now < st->mNextTick) return;
    st->mNextTick = now + SSBC7_MANIFEST_TICK_SECONDS;

    drainLoad(st);
    warmPass(st, now);
    verifyPass(st, now);

    if (now >= st->mNextRecord)
    {
        st->mNextRecord = now + (F64)SSBC7_MANIFEST_RECORD_SECONDS;
        recordSweep(st);
    }

    maybeLog(st, now);
}

void ssBC7ManifestBeginShutdown()
{
    ManifestState* st = state();
    if (!st) return;

    // The last region of a session is written HERE rather than in shutdown, because the pool that does the writing is closed by the time shutdown runs and a session that ended somewhere new would otherwise have learned nothing about it.
    if (st->mEnabled.load() && st->mHandle && !st->mDir.empty()) postSave(st);
}

void ssBC7ManifestShutdown()
{
    ManifestState* st = state();
    if (!st) return;

    st->mShuttingDown = true;

    // Deliberately never deleted, exactly as the encode and serve states are not: a pool worker that already loaded this pointer must find a shutting-down state rather than freed memory.
    st->mPending.clear();
    st->mWarmQueue.clear();
}

SSBC7ManifestStats ssBC7ManifestStatsNow()
{
    ManifestState* st = state();
    if (!st) return SSBC7ManifestStats();

    SSBC7ManifestStats out = st->mStats;
    out.mManifestsSaved = st->mSaved.load();
    return out;
}

std::string ssBC7ManifestMetricsString()
{
    ManifestState* st = state();
    if (!st) return "BC7 manifests: not initialised";

    std::string verdicts;
    for (U32 i = 0; i < SSBC7_MANIFEST_VERDICT_COUNT; ++i)
    {
        const U32 n = st->mVerdicts[i].load();
        if (!n) continue;
        if (!verdicts.empty()) verdicts += ", ";
        verdicts += llformat("%s %u", ssBC7ManifestVerdictName((ESSBC7ManifestVerdict)i), n);
    }
    if (verdicts.empty()) verdicts = "no manifest has been read yet";

    const SSBC7ManifestStats s = ssBC7ManifestStatsNow();

    return llformat("BC7 manifests %s | loaded %u (%u textures named), refused %u, saved %u, save failures %u | pre-warm issued %u, landed %u (%llu MB BC7, %llu MB saved), already resident %u, no record %u, declined %u, reader busy %u | recording %u of %u uuids for this region, %u dropped by the cap this session (so every manifest written since is a FLOOR)",
                    verdicts.c_str(),
                    s.mManifestsLoaded, s.mEntriesLoaded, s.mManifestsRefused, s.mManifestsSaved, st->mSaveFailed.load(),
                    s.mWarmIssued, s.mWarmLanded,
                    (U64)(s.mWarmBytesInPlace >> 20), (U64)(s.mWarmBytesSaved >> 20),
                    s.mWarmAlreadyResident, s.mWarmNoRecord, s.mWarmDeclined, s.mWarmRefused,
                    s.mRecorded, (U32)SSBC7_MANIFEST_MAX_ENTRIES, s.mRecordDropped);
}
