/**
 * @file ssbc7promote.cpp
 * @brief Squeeze promotion engine - fills the BC7 tier from J2C already on disk first, and only then from the network, see doc/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssbc7promote.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llimagej2c.h"
#include "llstartup.h"
#include "lltexturecache.h"
#include "lltexturefetch.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewerdisplay.h"        // gTeleportDisplay
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "ssbc7adaptive.h"          // <SS:Nexii/> Squeeze adaptive quality - the controller decides both which profile a promotion encodes at and whether the machine is calm enough to redo old work
#include "ssbc7encodequeue.h"
#include "ssbc7encoder.h"
#include "ssbc7manifest.h"          // <SS:Nexii/> Squeeze region manifests - the tick below drives the region-change detector
#include "ssbc7serve.h"
#include "ssbc7store.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>            // <SS:Nexii/> Squeeze adaptive quality - the upgrade tier's write-off list

// THE SHAPE, and why each half runs where it does.
//
// The SCAN and the FUSED ENCODE run on the existing BC7Encode pool because both do blocking disk IO and hundreds of milliseconds of CPU, and because sharing the demand path's pool is what guarantees a backfill encode can never be scheduled ahead of a texture the user is looking at. The TICK and every network decision run on the MAIN thread, because gTextureList, gTeleportDisplay, the startup state and LLTextureFetch::createRequest are all main-thread affairs and because a promotion fetch has to be able to see the same "is the fetcher busy" answer the texture list itself acts on. Nothing here ever blocks either thread: every post is a tryPost whose refusal is a counted verdict, and the main thread's whole cost on a pass is a handful of map lookups.
//
// LOCK ORDERING, and why this engine does not disturb it. LLTextureCache holds mHeaderMutex across clearCorruptedCache, which calls purgeAllTextures, which takes SSBC7Store's locks - so mHeaderMutex before the store's locks is an ordering that already exists in the tree. This module never nests the two in either direction: ssProbeJ2C takes mHeaderMutex, releases it, and only then does the encode and the store append, so there is no path from a store lock into the header mutex and the existing order is left exactly as it was. The body read is likewise outside mHeaderMutex; the only thing held under it is the same single fixed-size entry read getHeaderCacheEntry already does from the cache thread.

namespace
{
    // How many textures one scan is allowed to hand to the store's dedupe check and the cache's entry table. Each probe is one small positional read under mHeaderMutex, which is a mutex the texture cache thread also wants, so the batch exists to keep this a short burst rather than a sweep.
    struct NetCandidate
    {
        LLUUID mID;
        U32    mMissing;    // bytes the entry table says are not on disk yet, which is what the bandwidth budget is charged
    };

    struct PromoteState
    {
        // Cached policy, set on the main thread. The scan worker reads mEnabled to bail out early; everything network-related is only ever read on the main thread.
        std::atomic<bool>   mEnabled{false};
        std::atomic<bool>   mNetworkEnabled{false};
        std::atomic<bool>   mShuttingDown{false};

        SSBC7Store*         mStore{nullptr};
        LLTextureCache*     mCache{nullptr};    // resolved on the main thread at init, because a worker must never be the first toucher of a viewer global

        // ---- main thread only ----
        SSBC7NetBudget      mBudget;
        F64                 mNextPassTime{0.0};
        F64                 mNextLogTime{0.0};

        struct NetRequest
        {
            LLUUID mID;
            F64    mStarted;
            U64    mEstimate;
        };
        std::vector<NetRequest> mNetInFlight;

        U32                 mNetIssued{0};
        U32                 mNetCompleted{0};
        U32                 mNetTimedOut{0};
        U32                 mNetAbandoned{0};
        U32                 mNetNoTarget{0};
        U32                 mReArmed{0};

        // ---- shared ----
        std::mutex              mMutex;
        std::deque<NetCandidate> mNetCandidates;   // oldest at the front, exactly as the want list; written by the scan worker, drained by the main thread
        U32                     mNetCandidatesDropped{0};

        // <SS:Nexii> Squeeze adaptive quality - uuids the upgrade tier has written off, guarded by mMutex. Capped, because a store whose J2C sources have all been cleared would otherwise grow one entry per record and then keep re-reading them anyway. Once the cap is reached nothing more is added, so the pass retries those textures - which is the right way round: refusing to remember a failure costs a wasted probe, while refusing to remember a success would cost a wasted encode.
        std::unordered_set<LLUUID> mUpgradeSkip;
        std::atomic<bool>          mUpgradeRunning{false};
        std::atomic<U32>           mUpgradesPosted{0};
        std::atomic<U32>           mUpgradeSkipped{0};
        // </SS:Nexii>

        // Atomic because a pool worker records its own verdicts - a fused encode that hit the pinned-bytes ceiling is an exit of this engine and has to be counted where the others are, not in a second tally nobody reads.
        std::atomic<U32>    mVerdicts[SSBC7_PROMOTE_VERDICT_COUNT];
        std::atomic<U32>    mLastVerdict{(U32)SSBC7_PROMOTE_IDLE_EMPTY};

        std::atomic<bool>   mScanRunning{false};
        // <SS:Nexii/> Squeeze capacity-driven promotion - fused local encodes posted by this engine that no worker has started, and those a worker is inside. The first is what keeps a pass from posting the same spare capacity twice: work sitting in the queue is not busy yet, but it is spoken for.
        std::atomic<S32>    mLocalQueued{0};
        std::atomic<S32>    mLocalRunning{0};
        std::atomic<U32>    mScans{0};
        std::atomic<U32>    mLocalPromoted{0};
        std::atomic<U32>    mLocalEncodeFailed{0};
        std::atomic<U32>    mLocalDecodeFailed{0};
        std::atomic<U32>    mLocalGeometry{0};
        std::atomic<U32>    mLocalAlready{0};
        std::atomic<U32>    mProbeNoEntry{0};
        std::atomic<U32>    mProbeUnknown{0};
        std::atomic<U32>    mProbeReadFailed{0};
        std::atomic<U32>    mProbeRaced{0};
        std::atomic<U32>    mProbePartial{0};
        std::atomic<U32>    mPostRefused{0};
        std::atomic<U32>    mBackpressure{0};

        // An array of atomics cannot take a member initialiser, so the constructor is what keeps the tally at zero.
        PromoteState()
        {
            for (U32 i = 0; i < SSBC7_PROMOTE_VERDICT_COUNT; ++i) mVerdicts[i] = 0;
        }
    };

    // Created once on the main thread and then never destroyed, for the same reason the encode queue's state is: a pool worker that loads this pointer while the viewer is quitting has to find a stopped engine rather than freed memory.
    std::atomic<PromoteState*> s_state{nullptr};

    PromoteState* state() { return s_state.load(std::memory_order_acquire); }

    // Any thread. The tally is what makes "off", "ran and declined everything" and "ran and did work" three different log lines rather than one silence.
    ESSBC7PromoteVerdict record(PromoteState* st, ESSBC7PromoteVerdict verdict)
    {
        ++st->mVerdicts[verdict];
        st->mLastVerdict.store((U32)verdict);
        return verdict;
    }

    // Pushes a partial texture onto the network candidate list. Bounded and NOISY when the bound bites, because a silently truncated candidate list is a promise that everything partial will eventually be completed when in fact most of it was thrown away.
    void noteNetCandidate(PromoteState* st, const LLUUID& id, U32 missing)
    {
        LLUUID dropped;
        bool   did_drop = false;
        U32    total_dropped = 0;

        {
            std::lock_guard<std::mutex> lock(st->mMutex);
            for (const NetCandidate& c : st->mNetCandidates)
            {
                if (c.mID == id) return;
            }
            while (st->mNetCandidates.size() >= SSBC7_PROMOTE_NET_CANDIDATES)
            {
                dropped  = st->mNetCandidates.front().mID;
                did_drop = true;
                st->mNetCandidates.pop_front();
                ++st->mNetCandidatesDropped;
            }
            NetCandidate c;
            c.mID      = id;
            c.mMissing = missing;
            st->mNetCandidates.push_back(c);
            total_dropped = st->mNetCandidatesDropped;
        }

        if (did_drop && (total_dropped == 1 || (total_dropped % 500) == 0))
        {
            LL_WARNS("Squeeze") << "BC7 network candidate list is at its cap of " << SSBC7_PROMOTE_NET_CANDIDATES
                                << ", so " << dropped << " was dropped to make room for " << id
                                << " - " << total_dropped << " dropped this session, oldest first, and those partial textures will not be completed unless they are seen again" << LL_ENDL;
        }
    }

    // ---- tier (a), on a BC7Encode pool worker ----

    // The fused item the design specifies: read a COMPLETE J2C off this disk, decode it, build the mip chain, encode and store, with no network involved at any step. It is one work item rather than a chain of them because the decode here has no latency consumer waiting on it, which is precisely what keeps it out of the ImageDecode pool where a latency inversion would be built in by construction.
    // <SS:Nexii> Squeeze adaptive quality - a candidate the upgrade tier could not finish. Written off rather than left in place, because the alternative is re-reading, re-decoding and re-refusing the same texture every single pass for the rest of the session - and the usual cause, a J2C that has been evicted from the texture cache, does not fix itself. Never applied to demand-path or fill work, which has a want list of its own for exactly this.
    void noteUpgradeGaveUp(PromoteState* st, const LLUUID& id)
    {
        {
            std::lock_guard<std::mutex> lock(st->mMutex);
            if (st->mUpgradeSkip.size() < SSBC7_ADAPT_UPGRADE_SKIP_CAP) st->mUpgradeSkip.insert(id);
        }
        ++st->mUpgradeSkipped;
        ssBC7AdaptiveNoteUpgradeFailed();
    }
    // </SS:Nexii>

    // <SS:Nexii> Squeeze adaptive quality - `quality` is the profile to encode at and `upgrade` says this is a re-encode of a record that already exists. The two are separate arguments rather than one, because the fill tier also has to name a profile and it is emphatically not superseding anything.
    void promoteOneLocal(PromoteState* st, const LLUUID& id, SSBC7Quality quality, bool upgrade)
    {
        // <SS:Nexii/> Squeeze adaptive quality - this function has nine exits, so the upgrade indicator is retired by a scope guard rather than by hand at each of them. An indicator that leaked would leave the overlay claiming an upgrade pass was running for the rest of the session.
        struct UpgradeMark
        {
            explicit UpgradeMark(bool on) : mOn(on) {}
            ~UpgradeMark() { if (mOn) ssBC7AdaptiveNoteUpgradeRunning(false); }
            bool mOn;
        } upgrade_mark(upgrade);

        // <SS:Nexii/> Squeeze capacity-driven promotion - a fill encode moves from "queued" to "running" the moment a worker picks it up, and leaves "running" on any of the exits below, again by scope guard. Upgrades are not counted: they are posted by their own tier against its own gate.
        struct LocalMark
        {
            explicit LocalMark(PromoteState* st, bool on) : mSt(st), mOn(on) { if (mOn) { --mSt->mLocalQueued; ++mSt->mLocalRunning; } }
            ~LocalMark() { if (mOn) --mSt->mLocalRunning; }
            PromoteState* mSt;
            bool mOn;
        } local_mark(st, !upgrade);

        if (ssBC7EncodeAbandonRequested()) { ssBC7EncodeUnclaim(id); return; }

        LLTextureCache* cache = st->mCache;
        // An upgrade never re-wants: the record already exists and is perfectly usable, so the want list - which is a list of textures with NO record - is the wrong place for it and putting it there would send the fill tier chasing something it already has.
        if (!cache) { ssBC7EncodeUnclaim(id); if (!upgrade) ssBC7EncodeWant(id); return; }

        std::vector<U8> j2c_bytes;
        S32 have = 0, total = 0;
        const LLTextureCache::ESSJ2CProbe probe = cache->ssProbeJ2C(id, true, j2c_bytes, have, total, nullptr);

        if (probe != LLTextureCache::SS_J2C_PROBE_COMPLETE || j2c_bytes.empty())
        {
            // Between the scan's probe and this read the entry can have been purged, reclaimed or found short. Every one of those is named by the probe itself; nothing here re-wants the texture, because the same read would simply fail again.
            switch (probe)
            {
                case LLTextureCache::SS_J2C_PROBE_NO_ENTRY:    ++st->mProbeNoEntry; break;
                case LLTextureCache::SS_J2C_PROBE_UNKNOWN:     ++st->mProbeUnknown; break;
                case LLTextureCache::SS_J2C_PROBE_READ_FAILED: ++st->mProbeReadFailed; break;
                case LLTextureCache::SS_J2C_PROBE_RACED:       ++st->mProbeRaced; break;
                default:                                       ++st->mProbeUnknown; break;
            }
            LL_DEBUGS("Squeeze") << "BC7 local promotion of " << id << " gave up: "
                                 << LLTextureCache::ssJ2CProbeName(probe) << LL_ENDL;
            if (upgrade) noteUpgradeGaveUp(st, id);   // <SS:Nexii/> Squeeze adaptive quality - the commonest outcome by far once a cache has been cleared, and the one that must not be retried forever
            ssBC7EncodeUnclaim(id);
            return;
        }

        LLPointer<LLImageJ2C> j2c = new LLImageJ2C();
        U8* dst = j2c->allocateData((S32)j2c_bytes.size());
        if (dst) memcpy(dst, &j2c_bytes[0], j2c_bytes.size());
        if (!dst || !j2c->updateData())
        {
            ++st->mLocalDecodeFailed;
            LL_WARNS("Squeeze") << "BC7 local promotion of " << id << " could not parse the cached JPEG2000 header, so the texture keeps using the ordinary path" << LL_ENDL;
            if (upgrade) noteUpgradeGaveUp(st, id);   // <SS:Nexii/> Squeeze adaptive quality
            ssBC7EncodeUnclaim(id);
            return;
        }

        const U32 width      = (U32)j2c->getWidth();
        const U32 height     = (U32)j2c->getHeight();
        const U32 components = (U32)j2c->getComponents();

        if (!ssBC7EncodeGeometryOK(width, height, components))
        {
            // Not a failure and not worth re-trying: the same texture would be refused identically every pass, so it leaves the want list for good and is counted where a person can see it.
            ++st->mLocalGeometry;
            LL_DEBUGS("Squeeze") << "BC7 local promotion of " << id << " declined on geometry: " << width << "x" << height
                                 << " with " << components << " components" << LL_ENDL;
            if (upgrade) noteUpgradeGaveUp(st, id);   // <SS:Nexii/> Squeeze adaptive quality - a stored record whose source no longer passes the geometry gate is a build-to-build change, and asking again every pass would not resolve it
            ssBC7EncodeUnclaim(id);
            return;
        }

        const S64 raw_bytes = (S64)width * (S64)height * (S64)components;

        // Reserved BEFORE the raw is allocated, because the reservation exists to bound exactly these bytes. The shared budget is the demand path's, so a promotion decode can never push total pinned memory past the one ceiling the whole tier has.
        if (!ssBC7EncodeReserveBytes(raw_bytes))
        {
            ++st->mBackpressure;
            record(st, SSBC7_PROMOTE_DECLINE_BACKPRESSURE);
            ssBC7EncodeUnclaim(id);
            // <SS:Nexii/> Squeeze adaptive quality - NOT written off and NOT re-wanted. The pinned-bytes ceiling is a passing condition, so the next idle pass simply finds this record still below target and picks it up again; writing it off here would abandon a repairable texture over a moment of memory pressure.
            if (!upgrade) ssBC7EncodeWant(id);
            return;
        }

        bool stored = false;
        {
            j2c->setDiscardLevel(0);   // FULL RESOLUTION ONLY: a record exists at discard 0 or it does not exist

            LLPointer<LLImageRaw> raw = new LLImageRaw((U16)width, (U16)height, (S8)components);
            if (raw->getData() && j2c->decode(raw.get(), 0.f)
                && (U32)raw->getWidth() == width && (U32)raw->getHeight() == height && raw->getData())
            {
                stored = ssBC7EncodeAndStore(id, raw, quality, upgrade);
                if (!stored) ++st->mLocalEncodeFailed;
            }
            else
            {
                ++st->mLocalDecodeFailed;
                LL_WARNS("Squeeze") << "BC7 local promotion of " << id << " failed to decode a JPEG2000 the cache reported as complete at "
                                    << width << "x" << height << ", so the texture keeps using the ordinary path" << LL_ENDL;
            }
        }

        ssBC7EncodeReleaseBytes(raw_bytes);
        ssBC7EncodeUnclaim(id);

        // <SS:Nexii> Squeeze adaptive quality - an upgrade that reached the store is counted by the store itself, since it is the only thing that can tell a real supersede from a re-encode the append refused for not being an improvement. An upgrade that did NOT reach the store is written off here, because the same J2C would produce the same refusal on every later pass.
        if (upgrade)
        {
            if (!stored) noteUpgradeGaveUp(st, id);
            return;
        }
        // </SS:Nexii>

        if (stored) ++st->mLocalPromoted;
    }

    // <SS:Nexii> Squeeze adaptive quality - THE THIRD TIER, on a pool worker. It runs only when the first two have nothing left, which is the engine's existing definition of idle, and it reuses every piece of machinery the fill tier already has: the same pool, the same claim, the same pinned-bytes budget and the same fused read-decode-encode item. The only thing new is where the uuids come from.
    //
    // The store walk happens HERE rather than on the main thread because it touches every live record, and half a millisecond is nothing on a background worker and is a visible thing to hand the frame thread every two seconds.
    void runUpgrade(PromoteState* st)
    {
        const SSBC7Quality target = ssBC7AdaptiveUpgradeTarget();

        std::vector<LLUUID> batch;
        U32 remaining = 0;

        if (st->mStore)
        {
            // st->mMutex is held across the store's index walk, which is the one place in this engine it is held for longer than a few microseconds. The only contenders are pool workers recording network candidates or writing off an upgrade, and blocking one of those briefly is cheaper than keeping a second copy of the write-off list in step. The store's own map lock is taken INSIDE this one and never the other way about, which is the whole of the ordering.
            std::lock_guard<std::mutex> lock(st->mMutex);
            st->mStore->takeUpgradeCandidates((U8)target, SSBC7_ADAPT_UPGRADE_PER_PASS, st->mUpgradeSkip, batch, remaining);
        }

        size_t posted = 0;
        for (const LLUUID& id : batch)
        {
            if (ssBC7EncodeAbandonRequested() || st->mShuttingDown.load()) break;
            if (!ssBC7EncodeClaim(id)) continue;   // the demand path or the fill tier owns this uuid right now, and losing the claim is not a failure

            PromoteState* cap = st;
            const LLUUID  cid = id;
            // Raised BEFORE the post and lowered inside promoteOneLocal, so the indicator covers the re-encode itself rather than only the scan that found it. Lowered again here on a refusal, because an item that was never posted will never lower it.
            ssBC7AdaptiveNoteUpgradeRunning(true);
            if (ssBC7EncodeTryPost([cap, cid, target]() { promoteOneLocal(cap, cid, target, true); }))
            {
                ++posted;
            }
            else
            {
                ssBC7AdaptiveNoteUpgradeRunning(false);
                // A full queue means the demand path is busy, which is the one condition under which redoing old work should give way entirely. Unwound and simply not retried this pass.
                ++st->mPostRefused;
                ssBC7EncodeUnclaim(id);
            }
        }

        if (posted)
        {
            st->mUpgradesPosted += (U32)posted;
            LL_DEBUGS("Squeeze") << "BC7 upgrade pass posted " << posted << " re-encodes at " << ssBC7QualityName(target)
                                 << ", " << remaining << " records still below it" << LL_ENDL;
        }

        st->mUpgradeRunning.store(false);
        ssBC7AdaptiveNoteUpgradeRunning(false);
    }
    // </SS:Nexii>

    // Examines a bounded slice of the want list, sorts it into "encode this now for free", "this one needs network" and "this one is never going to happen", and posts the free ones. Runs wholly on a pool worker: every probe is blocking disk IO under the texture cache's header mutex.
    // <SS:Nexii/> Squeeze capacity-driven promotion - `budget` is the number of spare workers the tick measured when it posted this scan, and is the most fused encodes the scan may post. The batch examined grows with it, because on a list that is mostly partials a fixed forty-eight would find fewer complete ones than there are cores to give them to.
    void runScan(PromoteState* st, size_t budget)
    {
        std::vector<LLUUID> batch;
        ssBC7EncodeTakeWanted(llmax(SSBC7_PROMOTE_SCAN_BATCH, budget * 4), batch);

        size_t posted = 0;
        std::vector<U8> unused;

        for (const LLUUID& id : batch)
        {
            if (ssBC7EncodeAbandonRequested() || st->mShuttingDown.load())
            {
                // Everything not yet looked at goes back, so a quit costs the pass and not the population.
                ssBC7EncodeWant(id);
                continue;
            }

            if (st->mStore && st->mStore->hasRecord(id))
            {
                // Already stored, usually because the demand path got there first. hasRecord also stamps the heat signal, which is correct: something wanted this texture recently enough to ask for it.
                ++st->mLocalAlready;
                continue;
            }

            if (posted >= budget)
            {
                // Every spare worker has been given something, so the rest of the batch goes back rather than being probed and then thrown away. Probing costs a header read each and the answer would be stale by the next pass anyway.
                ssBC7EncodeWant(id);
                continue;
            }

            S32 have = 0, total = 0;
            const LLTextureCache::ESSJ2CProbe probe = st->mCache
                ? st->mCache->ssProbeJ2C(id, false, unused, have, total, nullptr)
                : LLTextureCache::SS_J2C_PROBE_NO_ENTRY;

            switch (probe)
            {
                case LLTextureCache::SS_J2C_PROBE_COMPLETE:
                {
                    if (!ssBC7EncodeClaim(id))
                    {
                        // Another worker already owns this uuid; the claim is the plain-uuid dedupe and losing it is not a failure.
                        ++st->mLocalAlready;
                        break;
                    }
                    PromoteState* cap = st;
                    const LLUUID  cid = id;
                    // <SS:Nexii/> Squeeze adaptive quality - the profile is read on the WORKER at the moment the encode starts, not captured here, so a fill encode that sat in the queue while the controller changed its mind is written at the profile that is current when it actually runs.
                    ++st->mLocalQueued;   // <SS:Nexii/> Squeeze capacity-driven promotion - counted BEFORE the post so a tick that runs between the post and the worker's pickup cannot see the slot as free
                    if (ssBC7EncodeTryPost([cap, cid]() { promoteOneLocal(cap, cid, ssBC7AdaptiveQualityNow(), false); }))
                    {
                        ++posted;
                    }
                    else
                    {
                        // tryPost refuses when the shared queue is full, which means the demand path is busy - the one condition under which backfill should give way. Unwound completely and retried next pass.
                        --st->mLocalQueued;
                        ++st->mPostRefused;
                        ssBC7EncodeUnclaim(id);
                        ssBC7EncodeWant(id);
                    }
                    break;
                }

                case LLTextureCache::SS_J2C_PROBE_PARTIAL:
                {
                    ++st->mProbePartial;
                    const U32 missing = (U32)llmax(0, total - have);
                    noteNetCandidate(st, id, missing);
                    break;
                }

                case LLTextureCache::SS_J2C_PROBE_NO_ENTRY:
                    // The J2C is not on this disk at all, so there is no partial to continue and nothing to encode. Dropped rather than re-wanted, because keeping it would make the want list a list of textures the engine can never act on.
                    ++st->mProbeNoEntry;
                    break;

                case LLTextureCache::SS_J2C_PROBE_UNKNOWN:
                    // The entry never learned the asset's total size, so neither tier can act: tier (a) cannot know it is complete and tier (b) cannot know how much to ask for.
                    ++st->mProbeUnknown;
                    break;

                case LLTextureCache::SS_J2C_PROBE_READ_FAILED:
                    ++st->mProbeReadFailed;
                    break;

                case LLTextureCache::SS_J2C_PROBE_RACED:
                    ++st->mProbeRaced;
                    ssBC7EncodeWant(id);
                    break;

                default:
                    ++st->mProbeUnknown;
                    break;
            }
        }

        ++st->mScans;
        if (posted) record(st, SSBC7_PROMOTE_RAN_LOCAL);
        st->mScanRunning.store(false);
    }

    // <SS:Nexii> Squeeze capacity-driven promotion - how many fused encodes tier (a) may post THIS pass, measured rather than configured.
    //
    // The number of cores with nothing to do: pool width, minus workers inside an encode at this instant (demand path and this engine alike), minus the fused encodes this engine has already posted that no worker has reached. The last term is what stops two passes a quarter second apart from filling the same idle worker twice. The scan itself briefly occupies a worker while it probes the cache, which is not subtracted because it is over in milliseconds and the encodes it posts queue behind it anyway.
    S32 spareWorkers(PromoteState* st)
    {
        const S32 width = ssBC7EncodePoolWidth();
        if (width <= 0) return 0;
        return width - ssBC7AdaptiveBusyWorkersNow() - llmax(0, st->mLocalQueued.load());
    }

    // Whether the adaptive controller is content, which is the system-stress signal: it measures what the pool is ACHIEVING against what is waiting, and other load on the machine shows up there as falling throughput exactly as texture volume does. HEADROOM is "keeping up easily" and the full spare count is offered. STEPPED_DOWN means the controller has positively found the machine behind, and nothing is added. HOLDING is the dead band - not behind, not comfortably ahead - and gets a trickle of ONE, which keeps fresh throughput samples flowing to the controller so it can decide; refusing outright there would let a stale measurement freeze the engine for as long as the measurement stayed stale. NO_DATA, PINNED and NO_BACKEND have no verdict to offer, so the spare count alone decides.
    S32 controllerBudget(S32 spare, ESSBC7PromoteVerdict& out_decline)
    {
        const SSBC7AdaptiveStats a = ssBC7AdaptiveStatsNow();
        switch ((ESSBC7AdaptReason)a.mReason)
        {
            case SSBC7_ADAPT_STEPPED_DOWN:
                out_decline = SSBC7_PROMOTE_DECLINE_NOT_KEEPING_UP;
                return 0;
            case SSBC7_ADAPT_HOLDING:
                return llmin(spare, 1);
            default:
                return spare;
        }
    }
    // </SS:Nexii>

    // ---- tier (b), main thread ----

    // Everything that has to be true before a single byte of the user's bandwidth is spent on a texture they are not looking at. Each answer is a named verdict, so a log line can say WHICH of them is holding the engine back rather than reporting a silent nothing.
    ESSBC7PromoteVerdict networkGate(PromoteState* st)
    {
        if (!st->mNetworkEnabled.load())                                    return SSBC7_PROMOTE_DECLINE_NET_OFF;
        // Login state is checked BEFORE the agent's teleport state, so nothing here reads gAgent until there is a session for it to describe.
        if (gDisconnected || LLStartUp::getStartupState() != STATE_STARTED) return SSBC7_PROMOTE_DECLINE_NET_LOGIN;
        if (gTeleportDisplay || gAgent.getTeleportState() != LLAgent::TELEPORT_NONE) return SSBC7_PROMOTE_DECLINE_NET_TELEPORT;
        // <SS:Nexii> The structural bounds live here: the want list empties, so the work is finite; the store budget caps what can be kept; the foreground check below and the in-flight limit keep this off the user's own fetches. The byte bucket and session ceiling (SSSqueezeNetworkPromoteKBPerSec / MaxMB, applied in issueOne through st->mBudget) are the user-facing dials on top of those, so a metered connection can cap what this tier spends; both were documented from the start and only wired on 2026-09-10, before which the tier never issued a fetch at all. doc/super_compressed_textures.md.
        if (st->mNetInFlight.size() >= (size_t)ssBC7PromoteMaxInFlight())   return SSBC7_PROMOTE_DECLINE_NET_IN_FLIGHT;

        LLTextureFetch* fetcher = LLAppViewer::getTextureFetch();
        if (!fetcher) return SSBC7_PROMOTE_DECLINE_NET_LOGIN;

        // FOREGROUND ALWAYS WINS. llviewertexturelist.cpp:924 treats zero outstanding requests as "the fetcher is idle"; a small allowance is kept only because a single lingering DONE worker would otherwise switch the engine off for a whole session. Our own requests are subtracted so the engine cannot mistake its own work for the user's.
        const S32 outstanding = fetcher->getNumRequests() - (S32)st->mNetInFlight.size();
        if (outstanding > SSBC7_PROMOTE_FETCH_IDLE_MAX) return SSBC7_PROMOTE_DECLINE_NET_BUSY;

        return SSBC7_PROMOTE_RAN_NETWORK;
    }

    // <SS:Nexii> Squeeze adaptive quality - whether the third tier may run at all, given how tier (b) ended. The three refusals that mean THE USER IS BUSY block it; every other outcome, including network promotion simply being switched off, leaves the machine idle and is exactly when old records should be improved. Reported as a reason rather than a bare bool so the log line names which half said no.
    bool upgradeMayRun(PromoteState* st, ESSBC7PromoteVerdict gate)
    {
        if (gate == SSBC7_PROMOTE_DECLINE_NET_BUSY
            || gate == SSBC7_PROMOTE_DECLINE_NET_TELEPORT
            || gate == SSBC7_PROMOTE_DECLINE_NET_LOGIN)
        {
            return false;
        }

        std::string reason;
        if (!ssBC7AdaptiveUpgradeAllowed(reason))
        {
            // Once, not every two seconds. The commonest case by far is the setting being off, which is a standing condition and not an event.
            if (st->mVerdicts[SSBC7_PROMOTE_IDLE_EMPTY].load() == 1)
            {
                LL_INFOS("Squeeze") << "BC7 idle re-encode of old records is standing by: " << reason << LL_ENDL;
            }
            return false;
        }
        return true;
    }
    // </SS:Nexii>

    // One continuation fetch. Returns true when a request went out.
    //
    // WHY THIS IS THE FETCH PIPELINE'S NATIVE MODE and not a bespoke transfer: createRequest at discard 0 asks for the whole asset, and LLTextureFetchWorker resumes from whatever is already in the cache using the offset arithmetic it uses for every other progressive fetch. The completed J2C is written back to the texture cache by the ordinary WRITE_TO_CACHE state and the encode is triggered by the ordinary DONE hook, so this module contributes no new transfer code, no new cache code and no new encode code - only the decision to ask.
    // <SS:Nexii> Squeeze network completion - throttled_out is SSBC7_PROMOTE_VERDICT_COUNT unless the budget was what stopped this candidate, in which case it names WHICH limit bit so the pass can report the throttle instead of reporting "no target", which is what a bandwidth stall used to look like from the diagnostics side.
    bool issueOne(PromoteState* st, const NetCandidate& cand, F64 now, ESSBC7PromoteVerdict& throttled_out)
    {
        LLTextureFetch* fetcher = LLAppViewer::getTextureFetch();
        if (!fetcher) return false;

        LLViewerFetchedTexture* tex = gTextureList.findImage(cand.mID, TEX_LIST_STANDARD);
        if (!tex)
        {
            // TARGETING. With manifests still unbuilt (P5), a live entry in the texture list is the strongest available statement that this texture was recently in front of the user - which is exactly the "manifest-referenced or recent last_use" rule the design asks for, expressed with what exists today. A texture the viewer has already forgotten is never worth bandwidth.
            ++st->mNetNoTarget;
            return false;
        }

        if (tex->getFTType() != FTT_DEFAULT || tex->needsAux())
        {
            ++st->mNetNoTarget;
            return false;
        }

        // Already known to be unencodable, so completing it would spend bandwidth for a record that could never be written. Dimensions are only trusted once they are known.
        const S32 w = tex->getFullWidth();
        const S32 h = tex->getFullHeight();
        if (w > 0 && h > 0 && !ssBC7EncodeGeometryOK((U32)w, (U32)h, (U32)llmax(1, (S32)tex->getComponents())))
        {
            ++st->mNetNoTarget;
            return false;
        }

        if (st->mStore && st->mStore->hasRecord(cand.mID))
        {
            ++st->mNetNoTarget;
            return false;
        }

        // The user's own fetch owns this texture right now. Never interfere with one: taking it over would change the discard the texture list asked for and put a full-resolution image into video memory that nothing wanted.
        if (tex->isFetching() || tex->hasFetcher())
        {
            ++st->mNetNoTarget;
            return false;
        }

        const U64 estimate = cand.mMissing > 0 ? (U64)cand.mMissing : (U64)1024;
        if (!st->mBudget.spend(estimate))
        {
            // <SS:Nexii> Squeeze network completion - a refusal by the budget is a DEFERRAL, not a rejection: the candidate goes back exactly where it was taken from so the next pass with credit still has it, and the reason is handed up so the pass records the throttle. The session ceiling is separated from the per-second bucket because they mean completely different things to a person reading the overlay - one clears in a second, the other does not clear until the viewer restarts.
            const U64 cap = st->mBudget.sessionCap();
            throttled_out = (cap != 0 && st->mBudget.spentTotal() + estimate > cap)
                          ? SSBC7_PROMOTE_DECLINE_NET_SESSION
                          : SSBC7_PROMOTE_DECLINE_NET_BANDWIDTH;
            {
                std::lock_guard<std::mutex> lock(st->mMutex);
                st->mNetCandidates.push_back(cand);
            }
            return false;
        }

        // Priority is a small positive number rather than zero: LLTextureFetchWorker aborts outright below F_ALMOST_ZERO (lltexturefetch.cpp:1158), and the UDP queue orders by this value, so one is the lowest number that still means "do this eventually" and sits below every priority the texture list ever assigns.
        const S32 rc = fetcher->createRequest(tex->getFTType(), tex->getUrl(), cand.mID, tex->getTargetHost(),
                                              1.f, 0, 0, 0, 0, false, true);
        if (rc < 0)
        {
            st->mBudget.refund(estimate);
            ++st->mNetNoTarget;
            LL_DEBUGS("Squeeze") << "BC7 network promotion of " << cand.mID << " was refused by the fetcher, code " << rc << LL_ENDL;
            return false;
        }

        PromoteState::NetRequest req;
        req.mID       = cand.mID;
        req.mStarted  = now;
        req.mEstimate = estimate;
        st->mNetInFlight.push_back(req);
        ++st->mNetIssued;

        LL_DEBUGS("Squeeze") << "BC7 network promotion asked for the remaining " << estimate << " bytes of " << cand.mID
                             << ", " << st->mNetInFlight.size() << " continuation fetches in flight" << LL_ENDL;
        return true;
    }

    // Retires finished, stalled and vanished continuation fetches. Runs every tick regardless of the gates, because it is cleanup rather than work - a slot held by a request nobody is watching is a slot the engine never gets back.
    void pollNetRequests(PromoteState* st, F64 now)
    {
        if (st->mNetInFlight.empty()) return;

        LLTextureFetch* fetcher = LLAppViewer::getTextureFetch();
        if (!fetcher)
        {
            st->mNetAbandoned += (U32)st->mNetInFlight.size();
            st->mNetInFlight.clear();
            return;
        }

        // A teleport wipes every in-flight fetch (llviewertexturelist.cpp:855-861 calls deleteAllRequests), so the workers behind these are already gone and holding the slots open would simply stall the engine until each one timed out.
        if (gTeleportDisplay)
        {
            st->mNetAbandoned += (U32)st->mNetInFlight.size();
            st->mNetInFlight.clear();
            return;
        }

        for (size_t i = 0; i < st->mNetInFlight.size();)
        {
            PromoteState::NetRequest& req = st->mNetInFlight[i];

            S32 discard = -1;
            S32 worker_state = 0;
            LLPointer<LLImageRaw> raw;
            LLPointer<LLImageRaw> aux;
            LLCore::HttpStatus status;

            bool retire = false;

            // The canonical "is this worker finished" question, and the same one LLViewerFetchedTexture asks. The raw it hands back is released the moment this scope ends: the encode already happened inside the worker's own DONE state, so nothing here needs the pixels.
            if (fetcher->getRequestFinished(req.mID, discard, worker_state, raw, aux, status))
            {
                const bool got_record = st->mStore && st->mStore->hasRecord(req.mID);
                if (got_record) ++st->mNetCompleted;
                else
                {
                    // Finished but no record: the DONE hook declined it, most often on geometry or because the asset turned out to be missing. Counted as a no-target rather than a completion, because a completion that produced nothing would make the success number a lie.
                    ++st->mNetNoTarget;
                }
                LL_DEBUGS("Squeeze") << "BC7 network promotion of " << req.mID << " finished at discard " << discard
                                     << ", record " << (got_record ? "stored" : "not stored") << LL_ENDL;
                retire = true;
            }
            else if ((now - req.mStarted) > (F64)SSBC7_PROMOTE_NET_TIMEOUT_SECS)
            {
                ++st->mNetTimedOut;
                LL_DEBUGS("Squeeze") << "BC7 network promotion of " << req.mID << " gave up after "
                                     << SSBC7_PROMOTE_NET_TIMEOUT_SECS << " seconds and its request was cancelled" << LL_ENDL;
                retire = true;
            }

            if (retire)
            {
                // Only ever deleted when the user's own texture has not taken the worker over in the meantime - both this and updateFetch run on the main thread, so checking here is enough to make that impossible rather than merely unlikely.
                LLViewerFetchedTexture* tex = gTextureList.findImage(req.mID, TEX_LIST_STANDARD);
                if (!tex || (!tex->isFetching() && !tex->hasFetcher()))
                {
                    fetcher->deleteRequest(req.mID, true);
                }
                st->mNetInFlight.erase(st->mNetInFlight.begin() + i);
            }
            else
            {
                ++i;
            }
        }
    }

    // A texture the read path probed before its record existed is left at DECLINED for NO_RECORD and never looks again, so without this the video memory the engine just spent CPU and bandwidth to make available would not be claimed until the next session.
    void reArmFreshlyStored(PromoteState* st)
    {
        std::vector<LLUUID> fresh;
        ssBC7EncodeTakeFreshlyStored(fresh);
        if (fresh.empty()) return;

        for (const LLUUID& id : fresh)
        {
            LLViewerFetchedTexture* tex = gTextureList.findImage(id, TEX_LIST_STANDARD);
            if (!tex) continue;

            if (tex->ssBC7Residency() == (U8)LLViewerFetchedTexture::SSBC7_RES_DECLINED
                && tex->ssBC7DeclineReason() == (U8)SSBC7_SERVE_DECLINE_NO_RECORD)
            {
                // Back to NONE and nothing further: the safety-net probe at the top of updateFetch is what re-examines it, so every exclusion is re-tested by the code that owns them rather than duplicated here.
                tex->ssBC7SetResidency((U8)LLViewerFetchedTexture::SSBC7_RES_NONE);
                ++st->mReArmed;
            }
        }
    }
}

// ---------------------------------------------------------------------------

void ssBC7PromoteInit()
{
    if (state()) return;

    PromoteState* st = new PromoteState();
    st->mStore = SSBC7Store::getInstance();
    st->mCache = LLAppViewer::getTextureCache();
    s_state.store(st, std::memory_order_release);

    ssBC7PromoteRefreshPolicy();
}

// <SS:Nexii> Four outstanding fetches per encode worker, which is what it takes to keep them fed given that a fetch costs several times what an encode does. Read live so raising SSSqueezeEncodeThreads raises this with it.
S32 ssBC7PromoteMaxInFlight()
{
    S32 workers = (S32)gSavedSettings.getU32("SSSqueezeEncodeThreads");
    if (workers <= 0)
    {
        const unsigned hw = std::thread::hardware_concurrency();
        workers = llclamp((S32)(hw / 4), 2, 16);
    }
    return llclamp(workers * 4, 8, 48);
}

void ssBC7PromoteRefreshPolicy()
{
    PromoteState* st = state();
    if (!st) return;

    st->mEnabled        = gSavedSettings.getBOOL("SSSqueezeEnabled") && gSavedSettings.getBOOL("SSSqueezePromote");
    st->mNetworkEnabled = gSavedSettings.getBOOL("SSSqueezeNetworkPromote");

    // <SS:Nexii> Squeeze network completion - the two limits tier (b) is documented to obey, applied here rather than at issue time so a changed setting takes effect on the very next pass and so configure() can clamp held credit down the instant the rate is lowered. Re-reading every refresh is safe on purpose: configure() sets the two limits and trims the token bucket and touches neither the session total nor the clock, so nobody can win a fresh session allowance by opening preferences. Without this call mRate stayed at its constructed zero, canSpend() refused everything, and the whole network tier was dead code that looked from the outside like "no target".
    const U64 net_rate = (U64)gSavedSettings.getU32("SSSqueezeNetworkPromoteKBPerSec") * 1024;
    const U64 net_cap  = (U64)gSavedSettings.getU32("SSSqueezeNetworkPromoteMaxMB") * 1024 * 1024;
    st->mBudget.configure(net_rate, net_cap);
    // </SS:Nexii>

    if (!st->mCache) st->mCache = LLAppViewer::getTextureCache();

    LL_INFOS("Squeeze") << "BC7 promotion policy: engine " << (st->mEnabled.load() ? "on" : "off")
                        << ", network completion " << (st->mNetworkEnabled.load() ? "on" : "off")
                        << " at up to " << (net_rate / 1024) << " KB/s"
                        << " and " << (net_cap / (1024 * 1024)) << " MB this session (spent "
                        << (st->mBudget.spentTotal() / (1024 * 1024)) << " MB so far)"
                        << ", up to " << ssBC7PromoteMaxInFlight()
                        << " continuation fetches at a time, paced by the texture HTTP policy class like every other fetch" << LL_ENDL;
}

void ssBC7PromoteTick()
{
    // <SS:Nexii> Squeeze region manifests - driven from here rather than from a new hook in llviewerregion.cpp, because this is already a main-thread tick and gAgent already knows which region the agent is in, so detecting the change costs one comparison and adds no stock surface at all. It sits ABOVE every gate below on purpose: manifests have their own setting and must not be switched off by SSSqueezePromote.
    ssBC7ManifestTick();
    // </SS:Nexii>

    PromoteState* st = state();
    if (!st || st->mShuttingDown.load()) return;

    const F64 now = LLTimer::getElapsedSeconds();
    if (now < st->mNextPassTime) return;
    st->mNextPassTime = now + (F64)SSBC7_PROMOTE_TICK_SECONDS;

    st->mBudget.advance(now);

    // Cleanup first and unconditionally. Both of these retire work the engine already started, and skipping them behind a gate is how a slot or a permanently declined texture is stranded when the user turns something off mid-session.
    reArmFreshlyStored(st);
    pollNetRequests(st, now);

    // The metrics line is written on a slow tick rather than per pass, so a session that promotes nothing still leaves one honest statement in the log and a busy one does not drown it.
    if (now >= st->mNextLogTime)
    {
        st->mNextLogTime = now + 120.0;
        LL_INFOS("Squeeze") << ssBC7PromoteMetricsString() << LL_ENDL;
        // <SS:Nexii/> Squeeze adaptive quality - written on the same slow tick rather than from a timer of its own, so a quiet session still leaves one honest statement of what the controller measured and chose, next to the engine whose backlog it was choosing against.
        LL_INFOS("Squeeze") << ssBC7AdaptiveMetricsString() << LL_ENDL;
    }

    if (!st->mEnabled.load())
    {
        record(st, SSBC7_PROMOTE_DECLINE_OFF);
        return;
    }

    std::string reason;
    if (!ssBC7EncodeGateReady(reason))
    {
        record(st, SSBC7_PROMOTE_DECLINE_NOT_READY);
        if (st->mVerdicts[SSBC7_PROMOTE_DECLINE_NOT_READY].load() == 1)
        {
            // Once, not every two seconds: this is usually a startup condition that resolves itself, and the count carries the rest of the story.
            LL_INFOS("Squeeze") << "BC7 promotion is standing by: " << reason << LL_ENDL;
        }
        return;
    }

    // Filling a store that is already over the user's budget only makes eviction drop something else, so the engine stops rather than spending CPU and bandwidth to churn the cache.
    if (st->mStore && st->mStore->budgetBytes() > 0 && st->mStore->allocatedBytes() > st->mStore->budgetBytes())
    {
        record(st, SSBC7_PROMOTE_DECLINE_STORE_FULL);
        return;
    }

    // THE IDLE RULE, and the answer to the question this engine exists for. Backfill runs only when the demand path has nothing in flight, so a texture the user is looking at is never behind a promotion encode in the shared queue.
    if (ssBC7EncodePendingCount() > 0)
    {
        record(st, SSBC7_PROMOTE_DECLINE_POOL_BUSY);
        return;
    }

    // ---- TIER (a): free work first, always ----
    if (ssBC7EncodeWantListSize() > 0)
    {
        // <SS:Nexii> Squeeze capacity-driven promotion - while there is work waiting the pass cadence is the FOLLOW-UP interval, not the idle one: a worker that finishes should be refilled within a frame or two, and the cost of looking is an atomic read and a size. The idle interval set at the top of the tick is left in place only by the exits that found nothing to do.
        st->mNextPassTime = now + SSBC7_PROMOTE_FOLLOWUP_SECONDS;

        if (st->mScanRunning.load())
        {
            record(st, SSBC7_PROMOTE_DECLINE_SCAN_RUNNING);
            return;
        }

        // How much may be added is measured on every pass: a spare core and a controller that is keeping up together admit one more fused encode, and only that. No fixed number anywhere.
        const S32 spare = spareWorkers(st);
        if (spare <= 0)
        {
            record(st, SSBC7_PROMOTE_DECLINE_NO_SPARE_WORKER);
            return;
        }
        ESSBC7PromoteVerdict decline = SSBC7_PROMOTE_DECLINE_NOT_KEEPING_UP;
        const S32 budget = controllerBudget(spare, decline);
        if (budget <= 0)
        {
            record(st, decline);
            return;
        }

        st->mScanRunning.store(true);
        PromoteState* cap = st;
        const size_t cap_budget = (size_t)budget;
        if (ssBC7EncodeTryPost([cap, cap_budget]() { runScan(cap, cap_budget); }))
        {
            record(st, SSBC7_PROMOTE_RAN_SCAN);
        }
        else
        {
            st->mScanRunning.store(false);
            ++st->mPostRefused;
            record(st, SSBC7_PROMOTE_DECLINE_POST_FAILED);
        }
        // </SS:Nexii>
        return;
    }

    // ---- TIER (b): only once tier (a) has nothing left at all ----
    const ESSBC7PromoteVerdict gate = networkGate(st);

    bool issued = false;
    // <SS:Nexii> Squeeze network completion - VERDICT_COUNT is the "no throttle happened" value; it is not a verdict and is never recorded, it only marks the slot as empty.
    ESSBC7PromoteVerdict throttled = SSBC7_PROMOTE_VERDICT_COUNT;
    if (gate == SSBC7_PROMOTE_RAN_NETWORK)
    {
        while (st->mNetInFlight.size() < (size_t)ssBC7PromoteMaxInFlight())
        {
            NetCandidate cand;
            {
                std::lock_guard<std::mutex> lock(st->mMutex);
                if (st->mNetCandidates.empty()) break;
                cand = st->mNetCandidates.back();     // newest first, for the same reason the want list hands out newest first
                st->mNetCandidates.pop_back();
            }

            if (issueOne(st, cand, now, throttled)) issued = true;
            // <SS:Nexii> Squeeze network completion - the loop MUST stop the moment the budget refuses, because issueOne puts that candidate straight back: carrying on would spin over the same deque forever rather than simply waiting for the next pass to hold credit.
            else if (throttled != SSBC7_PROMOTE_VERDICT_COUNT) break;
        }

        if (issued)
        {
            record(st, SSBC7_PROMOTE_RAN_NETWORK);
            return;
        }
    }

    bool candidates_left = false;
    {
        std::lock_guard<std::mutex> lock(st->mMutex);
        candidates_left = !st->mNetCandidates.empty();
    }

    // ---- TIER (c): nothing left to CREATE, so improve what is already there ----
    //
    // <SS:Nexii> Squeeze adaptive quality - this is the real answer to "if it is a slow day, use the best profile": not merely encoding new work better, but going back over what was rushed. It sits below both other tiers because a texture with no record at all is worth more than a texture with a merely imperfect one, and it reuses this engine's existing idle detection - the pool-busy check above - rather than standing up a second scheduler.
    //
    // Deliberately NOT run when the gate said the user is actively doing something. The pool being free is not the same as the machine being calm: a teleport arrival has an idle encode pool for the second before the textures land, and spending that second on old records is how a backfill ends up in front of the thing the user is looking at.
    if (upgradeMayRun(st, gate) && !st->mUpgradeRunning.exchange(true))
    {
        ssBC7AdaptiveNoteUpgradeRunning(true);

        PromoteState* cap = st;
        if (ssBC7EncodeTryPost([cap]() { runUpgrade(cap); }))
        {
            record(st, SSBC7_PROMOTE_RAN_UPGRADE);
            return;
        }

        st->mUpgradeRunning.store(false);
        ssBC7AdaptiveNoteUpgradeRunning(false);
        ++st->mPostRefused;
        record(st, SSBC7_PROMOTE_DECLINE_POST_FAILED);
        return;
    }
    // </SS:Nexii>

    if (gate != SSBC7_PROMOTE_RAN_NETWORK)
    {
        record(st, gate);
        return;
    }

    // <SS:Nexii> Squeeze network completion - the throttle outranks the fall-through verdict, because "the budget is empty" and "nothing on the list was worth fetching" are the two answers a person most needs told apart: one is the setting doing its job and will clear by itself, the other means targeting rejected everything and raising the limits would change nothing.
    if (throttled != SSBC7_PROMOTE_VERDICT_COUNT)
    {
        record(st, throttled);
        return;
    }
    // </SS:Nexii>

    record(st, candidates_left ? SSBC7_PROMOTE_DECLINE_NET_NO_TARGET : SSBC7_PROMOTE_IDLE_EMPTY);
}

void ssBC7PromoteBeginShutdown()
{
    PromoteState* st = state();
    if (!st) return;

    st->mShuttingDown = true;
}

void ssBC7PromoteShutdown()
{
    PromoteState* st = state();
    if (!st) return;

    st->mShuttingDown = true;

    // Every continuation fetch this module owns is cancelled outright. A quit must never wait on a download nobody asked for, and these are by definition the requests nobody asked for.
    if (LLTextureFetch* fetcher = LLAppViewer::getTextureFetch())
    {
        for (const PromoteState::NetRequest& req : st->mNetInFlight)
        {
            fetcher->deleteRequest(req.mID, true);
        }
    }
    st->mNetAbandoned += (U32)st->mNetInFlight.size();
    st->mNetInFlight.clear();

    LL_INFOS("Squeeze") << "BC7 promotion stopped. " << ssBC7PromoteMetricsString() << LL_ENDL;
}

SSBC7PromoteStats ssBC7PromoteStatsNow()
{
    SSBC7PromoteStats out;

    PromoteState* st = state();
    if (!st) return out;

    out.mLocalPromoted    = st->mLocalPromoted.load();
    out.mNetworkCompleted = st->mNetCompleted;
    out.mWaiting          = (U32)ssBC7EncodeWantListSize();
    out.mNetInFlight      = (U32)st->mNetInFlight.size();
    out.mNetIssued        = st->mNetIssued;
    out.mNetBytesSpent    = st->mBudget.spentTotal();
    out.mNetBytesCap      = st->mBudget.sessionCap();
    out.mLastVerdict      = st->mLastVerdict.load();
    // <SS:Nexii/> Squeeze adaptive quality - the third tier's activity. What is LEFT to upgrade lives in SSBC7AdaptiveStats, which reads the store's incremental histogram rather than counting anything here.
    out.mUpgradesPosted   = st->mUpgradesPosted.load();
    out.mUpgradeSkipped   = st->mUpgradeSkipped.load();
    out.mLocalQueued      = (U32)llmax(0, st->mLocalQueued.load());    // <SS:Nexii/> Squeeze capacity-driven promotion
    out.mLocalRunning     = (U32)llmax(0, st->mLocalRunning.load());

    // The two drop counts together, because the number still waiting is a FLOOR rather than a total whenever either cap has bitten, and an overlay that showed only "waiting" would say the engine had everything in hand.
    U32 cand_dropped = 0;
    {
        std::lock_guard<std::mutex> lock(st->mMutex);
        cand_dropped = st->mNetCandidatesDropped;
        out.mNetCandidates = (U32)st->mNetCandidates.size();
    }
    out.mWantDropped = ssBC7EncodeWantDroppedTotal() + cand_dropped;
    return out;
}

std::string ssBC7PromoteMetricsString()
{
    PromoteState* st = state();
    if (!st) return "BC7 promotion not started";

    std::string verdicts;
    for (U32 i = 0; i < SSBC7_PROMOTE_VERDICT_COUNT; ++i)
    {
        const U32 n = st->mVerdicts[i].load();
        if (!n) continue;
        if (!verdicts.empty()) verdicts += ", ";
        verdicts += llformat("%s %u", ssBC7PromoteVerdictName((ESSBC7PromoteVerdict)i), n);
    }
    if (verdicts.empty()) verdicts = "no pass has run yet";

    size_t candidates = 0;
    U32    cand_dropped = 0;
    {
        std::lock_guard<std::mutex> lock(st->mMutex);
        candidates   = st->mNetCandidates.size();
        cand_dropped = st->mNetCandidatesDropped;
    }

    return llformat("BC7 promotion: %u promoted from local cache, %u completed over the network, %u still waiting, %u found partial and %u held for network completion (%u dropped at the cap) | scans %u, already stored %u, no cache entry %u, size unknown %u, read failed %u, raced %u, geometry %u, decode failed %u, encode failed %u, post refused %u, backpressure %u | network issued %u, timed out %u, abandoned %u, no target %u, %llu of %llu MB spent | read path re-armed %u | last pass: %s | %s",
                    st->mLocalPromoted.load(),
                    st->mNetCompleted,
                    (U32)ssBC7EncodeWantListSize(),
                    st->mProbePartial.load(),
                    (U32)candidates,
                    cand_dropped,
                    st->mScans.load(),
                    st->mLocalAlready.load(),
                    st->mProbeNoEntry.load(),
                    st->mProbeUnknown.load(),
                    st->mProbeReadFailed.load(),
                    st->mProbeRaced.load(),
                    st->mLocalGeometry.load(),
                    st->mLocalDecodeFailed.load(),
                    st->mLocalEncodeFailed.load(),
                    st->mPostRefused.load(),
                    st->mBackpressure.load(),
                    st->mNetIssued,
                    st->mNetTimedOut,
                    st->mNetAbandoned,
                    st->mNetNoTarget,
                    (unsigned long long)(st->mBudget.spentTotal() / (1024 * 1024)),
                    (unsigned long long)(st->mBudget.sessionCap() / (1024 * 1024)),
                    st->mReArmed,
                    ssBC7PromoteVerdictName((ESSBC7PromoteVerdict)st->mLastVerdict.load()),
                    verdicts.c_str());
}
