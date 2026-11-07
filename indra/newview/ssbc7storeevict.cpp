/**
 * @file ssbc7storeevict.cpp
 * @brief Squeeze BC7 sidecar store, eviction half - what dies, why, and the accounting that makes SSBC7CacheSize mean something, see doc/viewer/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssbc7store.h"

// Third of the store's three files, and the only one that decides anything. ssbc7store.cpp knows what the bytes MEAN and depends on nothing; ssbc7storeio.cpp knows where they GO and owns every handle, every path and the one delete; this file knows which of them are worth keeping. It deliberately performs no file IO of its own - it calls appendIndexRecords and killSegmentFile and nothing else - so that the answer to "what can this feature delete" stays confined to one function in one file.
//
// RECLAIM IS ALWAYS ONE WHOLE SEGMENT. Blob bytes are never read, moved or rewritten: the writer is a pure log and records are immutable per (uuid, encoder_version), so a segment is exactly "the textures first seen during one window of time" and contains no overwrite garbage a copy-forward pass could harvest. Copying live blobs out of a doomed segment would therefore spend real read and write IO on data it merely BELIEVES is hot, while dropping the record spends CPU only on the blobs demand later proves hot - and it spends it on a decode the viewer was doing anyway, because ssBC7EncodeConsider fires on every full-resolution DONE decode and is suppressed only by hasRecord(). Re-encode IS this design's copy phase, performed lazily and only for what came back.

#include "llappviewer.h"
#include "lldir.h"
#include "llfile.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace
{
    const char* s_evict_verdict_names[SSBC7_EVICT_COUNT] =
    {
        "killed a segment", "under budget", "feature off", "read only second instance", "store not ready",
        "store changed underneath", "already running", "cooling down", "budget setting unusable",
        "no sealed segment", "everything is hot", "still referenced", "index write failed", "unlink failed"
    };

    // One record, flattened for ranking. Copied out under the map lock and sorted outside it, because sorting tens of thousands of entries is exactly the kind of work that has no business holding the lock the texture fetch thread takes on every decode.
    struct SSBC7EvictEntry
    {
        U32 mTick;
        U32 mSize;
        U16 mSegment;
        U16 mDay;
        bool mHot;
    };

    // Drop priority, coldest first. The day is the only half that survives a restart; the session tick is what breaks the tie inside a single day, which on a cache filled in one sitting is EVERY record - a day-only sort there is not a ranking at all, it is whatever order the hash table happened to be in.
    bool ssBC7ColderFirst(const SSBC7EvictEntry& a, const SSBC7EvictEntry& b)
    {
        if (a.mDay != b.mDay) return a.mDay < b.mDay;
        return a.mTick < b.mTick;
    }

    // The hard recency floor, which is deliberately independent of mLastUseDay: a U16 of days cannot express "a minute ago", and a minute ago is precisely the window where dropping a record is most visibly wrong.
    bool ssBC7IsHot(U32 touch_second, U32 now)
    {
        if (!touch_second) return false;                 // never referenced this session, and a zero here is also what every record loaded from disk starts as
        if (now < touch_second) return true;             // the clock moved backwards; treat the record as hot, because guessing cold is the destructive direction
        return (now - touch_second) < SSBC7_EVICT_HOT_SECONDS;
    }
}

const char* ssBC7EvictVerdictName(ESSBC7EvictVerdict verdict)
{
    if ((U32)verdict >= (U32)SSBC7_EVICT_COUNT) return "unknown";
    return s_evict_verdict_names[verdict];
}

// <SS:Nexii> Squeeze eviction - segment ids are consumed at the rate the store is WRITTEN rather than the rate it grows, so with a runtime cap of tens of megabytes and monotonic ids the 4096 id space is spent in about a day of active exploring, after which appendBlob refuses for the whole remaining session with nothing but a warning. Reuse is therefore load bearing, not a refinement. Nothing about the free list is persisted: at startup it is derived as every id below the write head that no live record points into.
bool SSBC7Store::allocateSegment(U32& out_segment)
{
    // Caller holds mStoreMutex.
    for (size_t i = 0; i < mFreeSegments.size(); ++i)
    {
        const U32 id = mFreeSegments[i];
        // A dying id is not free until its kill has finished, or the allocator would hand out the very segment that is about to be unlinked. Kept sorted, so this is lowest-free.
        if (id >= SSBC7_MAX_SEGMENTS || id == mDyingSegment || id == mCurrentSegment) continue;
        mFreeSegments.erase(mFreeSegments.begin() + (std::ptrdiff_t)i);
        out_segment = id;
        return true;
    }

    if (mSegmentHigh + 1 >= SSBC7_MAX_SEGMENTS) return false;
    out_segment = ++mSegmentHigh;
    return true;
}

// The reference signal's other half. A texture that decodes once at login and is then permanently resident - avatar skins, the UI atlas, system assets - never reaches the encode gate again, so a fetch-time-only signal would rank exactly the textures the user is looking at every second of the session as the coldest things in the store, which is precisely backwards.
void SSBC7Store::touchReferenced(const std::vector<LLUUID>& ids, std::vector<LLUUID>& out_rewant)
{
    out_rewant.clear();
    if (!mInitialized || mShuttingDown) return;

    const U16 today = ssBC7Today();
    const U32 now   = ssBC7NowSeconds();

    std::lock_guard<std::mutex> lock(mMapMutex);
    for (const LLUUID& id : ids)
    {
        auto it = mIndex.find(id);
        if (it != mIndex.end() && !it->second.isTombstone())
        {
            it->second.mLastUseDay  = today;
            it->second.mTouchTick   = ++mSessionTick;
            it->second.mTouchSecond = now;
            continue;
        }

        // Referenced right now and NOT in the store. Only the subset a kill dropped THIS session is worth handing back to the promotion engine - offering it every dropped uuid would send it to re-fetch exactly the textures that were just judged cold, which is the opposite of the point.
        if (out_rewant.size() < SSBC7_EVICT_REWANT_CAP && mDroppedUUIDs.count(id)) out_rewant.push_back(id);
    }
}

// One pass either kills exactly one segment or explains itself. The in-progress flag is taken here rather than by whoever posts the work, so that "someone else is already doing this" is a counted verdict like every other refusal instead of a work item that silently evaporates.
ESSBC7EvictVerdict SSBC7Store::evictPass(bool respect_cooldown)
{
    ++mMetrics.mEvictPasses;

    bool expected = false;
    if (!mEvictRunning.compare_exchange_strong(expected, true))
    {
        ++mMetrics.mEvictVerdicts[SSBC7_EVICT_ALREADY_RUNNING];
        return SSBC7_EVICT_ALREADY_RUNNING;
    }

    const ESSBC7EvictVerdict verdict = evictPassLocked(respect_cooldown);

    mEvictRunning = false;
    ++mMetrics.mEvictVerdicts[verdict];
    return verdict;
}

ESSBC7EvictVerdict SSBC7Store::evictPassLocked(bool respect_cooldown)
{
    if (!enabled())                     return SSBC7_EVICT_DISABLED;
    if (!mInitialized || mShuttingDown) return SSBC7_EVICT_NOT_READY;
    if (mReadOnly)                      return SSBC7_EVICT_READ_ONLY;

    // purgeAll deletes the whole directory on the main thread and resets the append cursor, and initStore can legitimately run more than once; either one moving underneath this pass makes every id, offset and cursor it is holding meaningless, so the generation is re-read at every step that touches the store rather than sampled once at the top.
    const U32 gen = mGeneration.load();

    const U64 budget = budgetBytes();
    if (!budget)
    {
        // Said once, then carried by the verdict tally in metricsString - a warning the tick repeats every minute for the rest of the session teaches the reader to filter the tag out, which is the same silence by another route.
        if (mMetrics.mEvictVerdicts[SSBC7_EVICT_BUDGET_INVALID].load() == 0)
        {
            LL_WARNS("Squeeze") << "BC7 eviction declined: SSBC7CacheSize is zero, and a budget the user did not ask for is a worse guess than doing nothing" << LL_ENDL;
        }
        return SSBC7_EVICT_BUDGET_INVALID;
    }

    // THE BUDGET IS ENFORCED ON ALLOCATED BYTES. mDataBytes is the sum of live blob sizes: it excludes alignment pads and it falls the instant a record is tombstoned while the disk does not move until the unlink, so a watermark built on it would let the store sit permanently above the slider and report itself as compliant.
    const U64 allocated = allocatedBytes();
    if (allocated <= budget)
    {
        mEvictWanted = false;
        return SSBC7_EVICT_NOT_NEEDED;
    }

    const U32 now = ssBC7NowSeconds();
    if (respect_cooldown)
    {
        const U32 last = mLastEvictSecond.load();
        if (last && now >= last && (now - last) < SSBC7_EVICT_COOLDOWN_SECS) return SSBC7_EVICT_COOLDOWN;
    }

    U32 head = 0;
    U32 dying = SSBC7_MAX_SEGMENTS;
    {
        std::lock_guard<std::mutex> lock(mStoreMutex);
        if (mGeneration.load() != gen) return SSBC7_EVICT_STALE;
        head  = mCurrentSegment;
        dying = mDyingSegment;
    }

    std::vector<SSBC7EvictEntry> entries;
    std::vector<U64> seg_total((size_t)SSBC7_MAX_SEGMENTS, (U64)0);
    std::vector<U64> seg_hot((size_t)SSBC7_MAX_SEGMENTS, (U64)0);

    {
        // The one O(records) walk in the pass. It runs at most once a minute and only while the store is over budget, which is a far better place to pay for ranking than the alternative of keeping a global ordering up to date on the fetch thread's hot path every time a texture is touched.
        std::lock_guard<std::mutex> lock(mMapMutex);
        entries.reserve(mIndex.size());
        for (const auto& entry : mIndex)
        {
            const SSBC7Record& rec = entry.second;
            if (rec.isTombstone()) continue;

            SSBC7EvictEntry e;
            e.mSegment = rec.mSegment;
            e.mDay     = rec.mLastUseDay;
            e.mTick    = rec.mTouchTick;
            e.mSize    = rec.mBlobSize;
            e.mHot     = ssBC7IsHot(rec.mTouchSecond, now);

            seg_total[e.mSegment] += e.mSize;
            if (e.mHot) seg_hot[e.mSegment] += e.mSize;
            entries.push_back(e);
        }
    }

    if (entries.empty())
    {
        LL_INFOS("Squeeze") << "BC7 eviction has nothing to reclaim: the store holds " << (allocated / (1024 * 1024))
                            << " MB against a " << (budget / (1024 * 1024)) << " MB budget but no live records at all, so the space is torn tails the next startup sweep will take" << LL_ENDL;
        return SSBC7_EVICT_NO_CANDIDATE;
    }

    // GLOBAL DOOM SET FIRST, PLACEMENT SECOND. Ranking store-wide and only then asking which segment holds the most of that ranking is what makes a hot record stranded among cold ones show up as a number in the log rather than as an invisible accident - scoring each segment against itself would rate a uniformly lukewarm segment identically to one holding the coldest tail in the store.
    std::sort(entries.begin(), entries.end(), ssBC7ColderFirst);

    const U64 want = llmax(SSBC7_EVICT_BATCH, allocated - budget);
    std::vector<U64> seg_doom((size_t)SSBC7_MAX_SEGMENTS, (U64)0);
    U64 doomed = 0;
    U32 doom_count = 0;

    for (const SSBC7EvictEntry& e : entries)
    {
        if (doomed >= want) break;
        if (e.mHot) continue;   // the recency floor never joins the doom set, whatever its day and tick say
        seg_doom[e.mSegment] += e.mSize;
        doomed += e.mSize;
        ++doom_count;
    }

    U32 victim     = SSBC7_MAX_SEGMENTS;
    F64 best_score = -1.0;
    U64 best_total = 0;
    U32 sealed     = 0;
    U32 hot_excluded = 0;

    // The lowest scoring sealed segment, remembered even when the hot bar excludes it. This is the bottom rung of the escalation ladder and the reason there is no state where the store declines forever while continuing to grow.
    U32 fallback       = SSBC7_MAX_SEGMENTS;
    F64 fallback_hot   = 2.0;
    U64 fallback_total = 0;

    for (U32 s = 0; s < SSBC7_MAX_SEGMENTS; ++s)
    {
        if (seg_total[s] == 0) continue;
        if (s == head || s == dying) continue;   // the write head is never a victim, and neither is a segment another kill is still working through
        ++sealed;

        const F64 hot_fraction = (F64)seg_hot[s] / (F64)seg_total[s];
        const F64 score        = (F64)seg_doom[s] / (F64)seg_total[s];

        if (hot_fraction < fallback_hot || (hot_fraction == fallback_hot && seg_total[s] > fallback_total))
        {
            fallback       = s;
            fallback_hot   = hot_fraction;
            fallback_total = seg_total[s];
        }

        if (hot_fraction * 100.0 > (F64)SSBC7_EVICT_HOT_PERCENT)
        {
            ++hot_excluded;
            continue;
        }

        // Tie broken by size, because two segments holding the same fraction of the cold tail are not equally worth killing - the bigger one buys more headroom for the same one kill.
        if (score > best_score || (score == best_score && seg_total[s] > best_total))
        {
            victim     = s;
            best_score = score;
            best_total = seg_total[s];
        }
    }

    if (fallback == SSBC7_MAX_SEGMENTS)
    {
        LL_INFOS("Squeeze") << "BC7 eviction has no sealed segment to reclaim: everything live is in the write head, so the store sits "
                            << ((allocated - budget) / (1024 * 1024)) << " MB over its " << (budget / (1024 * 1024))
                            << " MB budget until the head rolls over" << LL_ENDL;
        return SSBC7_EVICT_NO_CANDIDATE;
    }

    if (victim == SSBC7_MAX_SEGMENTS)
    {
        // THE ESCALATION LADDER, STATED RATHER THAN IMPLIED: relax the hot bar only once the hard cap is in sight, then kill the lowest scoring segment anyway and say in the log that that is what happened. Ordinary overshoot still gets the benefit of the doubt, because a user parked in one busy region can legitimately have every sealed segment over the bar and dropping what they are looking at right now is worse than sitting a little over the slider.
        if (allocated <= budget + 2 * mSegmentCap)
        {
            LL_INFOS("Squeeze") << "BC7 eviction declined: all " << hot_excluded << " of " << sealed << " sealed segments are more than "
                                << SSBC7_EVICT_HOT_PERCENT << "% referenced in the last " << SSBC7_EVICT_HOT_SECONDS
                                << " seconds, so the store is sitting " << ((allocated - budget) / (1024 * 1024))
                                << " MB over its " << (budget / (1024 * 1024)) << " MB budget rather than dropping something in use" << LL_ENDL;
            return SSBC7_EVICT_ALL_HOT;
        }

        victim = fallback;
        LL_WARNS("Squeeze") << "BC7 eviction relaxed the hot bar: " << (allocated / (1024 * 1024)) << " MB is past the hard cap of "
                            << ((budget + 2 * mSegmentCap) / (1024 * 1024)) << " MB and every sealed segment is hot, so segment "
                            << victim << " dies anyway at " << (U32)(fallback_hot * 100.0) << "% recently referenced" << LL_ENDL;
    }

    // Recomputed from the victim that was actually chosen, because the escalation ladder can pick a segment the scoring loop never scored and a log line that reported the unreached best score would be describing a different segment.
    const F64 victim_score = (F64)seg_doom[victim] / (F64)llmax(seg_total[victim], (U64)1);

    // ---------------------------------------------------------------------
    // The kill. One durability point, and a valid store on both sides of it.
    // ---------------------------------------------------------------------

    {
        std::lock_guard<std::mutex> lock(mStoreMutex);
        if (mGeneration.load() != gen || mShuttingDown) return SSBC7_EVICT_STALE;
        if (victim == mCurrentSegment) return SSBC7_EVICT_NO_CANDIDATE;      // the head rolled over onto our victim while we were scoring
        if (mDyingSegment != SSBC7_MAX_SEGMENTS) return SSBC7_EVICT_ALREADY_RUNNING;
        mDyingSegment = victim;
    }

    // Every exit from here on has to put the id back, or the allocator loses it for the rest of the session and the segment can never be reclaimed again.
    auto releaseVictim = [this]()
    {
        std::lock_guard<std::mutex> lock(mStoreMutex);
        mDyingSegment = SSBC7_MAX_SEGMENTS;
    };

    std::vector<SSBC7Record> tombstones;
    U64 dropped_bytes = 0;
    U32 hot_dropped = 0;

    {
        std::lock_guard<std::mutex> lock(mMapMutex);

        // mSegMembers is a scoring accelerator and is allowed to lag; the map itself is the only authority for what a kill erases, because "delete a file nothing points at" is the one invariant this design cannot get wrong.
        for (auto it = mIndex.begin(); it != mIndex.end(); )
        {
            if (it->second.mSegment != (U16)victim) { ++it; continue; }

            SSBC7Record ts;
            ts.mUUID    = it->first;
            ts.mSegment = (U16)victim;
            ts.mFlags   = SSBC7_FLAG_TOMBSTONE;
            tombstones.push_back(ts);

            dropped_bytes += it->second.mBlobSize;
            if (ssBC7IsHot(it->second.mTouchSecond, now)) ++hot_dropped;

            // <SS:Nexii/> Squeeze adaptive quality - a kill is the other direction the histogram moves in, and it has to be adjusted here rather than recomputed, because this is already the walk that knows exactly which records are leaving.
            {
                const U8 q = it->second.mQuality;
                if (q < SSBC7_QUALITY_LEVELS && mQualityCounts[q]) --mQualityCounts[q];
            }

            // Capped, and it exists purely so re-encode-after-eviction can be MEASURED. Re-encode is this design's copy phase; if it ever costs more than a small fraction of bytes written then copy-forward or an explicit tenured generation was the better answer after all, and this counter is how that gets noticed instead of assumed.
            if (mDroppedUUIDs.size() < SSBC7_EVICT_DROPPED_CAP) mDroppedUUIDs.insert(it->first);

            it = mIndex.erase(it);
        }
        mSegMembers.erase((U16)victim);

        const U32 n = (U32)tombstones.size();
        mLiveRecords = (mLiveRecords.load() > n) ? (mLiveRecords.load() - n) : 0;
        mDataBytes   = (mDataBytes.load() > dropped_bytes) ? (mDataBytes.load() - dropped_bytes) : 0;
    }

    bool stale     = false;
    bool index_ok  = true;
    if (!tombstones.empty())
    {
        // THE ORDERING INVARIANT: tombstone, flush, and only then touch the file. At every instant a record is either live with its bytes intact or dead with its bytes reclaimable, never both. No fsync - every crash ordering collapses to the same read-side uuid and checksum check on the way back in, so the extra barrier would buy nothing but SSD wear.
        //
        // The outcome is carried out of the lock rather than acted on inside it, because releasing the victim id takes the very mutex this scope holds and std::mutex is not recursive.
        std::lock_guard<std::mutex> lock(mStoreMutex);
        if (mGeneration.load() != gen || mShuttingDown) stale = true;
        else                                            index_ok = appendIndexRecords(tombstones);
    }

    if (stale)
    {
        releaseVictim();
        return SSBC7_EVICT_STALE;
    }

    if (!index_ok)
    {
        // Nothing on disk changed. The records are gone from this process's map and will simply be back at the next startup, which is the conservative direction to fail in.
        LL_WARNS("Squeeze") << "BC7 eviction abandoned segment " << victim << ": the " << tombstones.size()
                            << " tombstones could not be written, so nothing on disk changed and the space stays until the next attempt" << LL_ENDL;
        releaseVictim();
        return SSBC7_EVICT_INDEX_WRITE_FAILED;
    }

    // THE POST-CONDITION ASSERT. "Never delete data that something still points at", checked rather than hoped for - and placed after the tombstone write on purpose, because that write is also what closes the window on an append which had already put a blob into this segment and had not yet reached the map.
    bool still_referenced = false;
    {
        std::lock_guard<std::mutex> lock(mMapMutex);
        for (const auto& entry : mIndex)
        {
            if (entry.second.mSegment == (U16)victim) { still_referenced = true; break; }
        }
    }

    if (still_referenced)
    {
        LL_WARNS("Squeeze") << "BC7 eviction stopped short of deleting segment " << victim
                            << ": a live record still points into it, so the file is left exactly where it is and the next pass will take it" << LL_ENDL;
        releaseVictim();
        return SSBC7_EVICT_STILL_REFERENCED;
    }

    // Outside every lock. The unlink is the only step whose latency is unbounded - NTFS can take tens of milliseconds over a large or fragmented file - and holding either mutex across it would stall the encode workers or the fetch thread for no reason at all.
    const bool unlinked = killSegmentFile(victim, gen);

    U64 reclaimed = 0;
    {
        std::lock_guard<std::mutex> lock(mStoreMutex);
        if (mGeneration.load() != gen)
        {
            // purgeAll already cleared the frontiers and the free list, so adding this id back would resurrect state belonging to a store that no longer exists. The dying marker is cleared anyway rather than reasoned about, since only one pass exists at a time and a stale marker would cost the new store an id forever.
            mDyingSegment = SSBC7_MAX_SEGMENTS;
            return SSBC7_EVICT_STALE;
        }

        reclaimed    = mSegFrontier[victim];
        mAllocBytes  = (mAllocBytes.load() > reclaimed) ? (mAllocBytes.load() - reclaimed) : 0;
        mSegFrontier[victim] = 0;

        // The id goes back on the free list whether or not the unlink worked. A failed unlink is NEVER rolled back: the records are already dead, so rolling back would have to un-kill them, and reusing the id simply overwrites the survivor from its header onwards - which reclaims the space just as well. ensureSegment rewrites the header of a reused file for exactly this case, and every read checks the blob's uuid, so the stale tail past the new write point can never be served as somebody else's texture.
        if (std::find(mFreeSegments.begin(), mFreeSegments.end(), victim) == mFreeSegments.end())
        {
            mFreeSegments.insert(std::lower_bound(mFreeSegments.begin(), mFreeSegments.end(), victim), victim);
        }
        mDyingSegment = SSBC7_MAX_SEGMENTS;
    }

    mLastEvictSecond = ssBC7NowSeconds();
    mEvictWanted     = false;

    ++mMetrics.mSegmentsKilled;
    mMetrics.mBytesReclaimed   += reclaimed;
    mMetrics.mRecordsDropped   += (U32)tombstones.size();
    mMetrics.mHotRecordsDropped += hot_dropped;
    if (!unlinked) ++mMetrics.mUnlinkFailures;

    LL_INFOS("Squeeze") << "BC7 eviction killed segment " << victim << ": " << (reclaimed / (1024 * 1024))
                        << " MB back from " << tombstones.size() << " records, " << hot_dropped
                        << " of them referenced in the last " << SSBC7_EVICT_HOT_SECONDS << " seconds. Chosen from "
                        << sealed << " sealed segments (" << hot_excluded << " excluded as hot) holding "
                        << (U32)(victim_score * 100.0) << "% of a " << (doomed / (1024 * 1024)) << " MB cold tail across "
                        << doom_count << " records. Store now " << (allocatedBytes() / (1024 * 1024)) << " MB of "
                        << (budget / (1024 * 1024)) << " MB"
                        << (unlinked ? "" : "; the file itself could not be deleted and is an orphan for the next startup sweep") << LL_ENDL;

    return unlinked ? SSBC7_EVICT_RAN : SSBC7_EVICT_UNLINK_FAILED;
}

// Startup only, before the store is published: no readers, no writers, nothing in flight, and the one moment where a budget the user lowered between sessions gets honoured without pushing IO into a live session. Bounded rather than unbounded because a slider dropped by an order of magnitude must cost a noticeable login, not an unbounded one - whatever is left over is simply taken by the watermark once the session is running.
void SSBC7Store::startupTrim()
{
    if (mReadOnly || mStoreDir.empty()) return;

    const U64 budget = budgetBytes();
    if (!budget)
    {
        ++mMetrics.mEvictVerdicts[SSBC7_EVICT_BUDGET_INVALID];
        LL_WARNS("Squeeze") << "BC7 startup trim skipped: SSBC7CacheSize is zero, so the store will grow until the setting is usable" << LL_ENDL;
        return;
    }

    const U64 before = allocatedBytes();
    if (before <= budget) return;

    U32 passes = 0;
    ESSBC7EvictVerdict last = SSBC7_EVICT_NOT_NEEDED;

    while (passes < SSBC7_EVICT_STARTUP_PASSES)
    {
        last = evictPass(false);
        ++passes;
        if (last != SSBC7_EVICT_RAN && last != SSBC7_EVICT_UNLINK_FAILED) break;
    }

    const U64 after = allocatedBytes();
    LL_INFOS("Squeeze") << "BC7 startup trim reclaimed " << ((before - after) / (1024 * 1024)) << " MB over " << passes
                        << " passes, taking the store from " << (before / (1024 * 1024)) << " MB to " << (after / (1024 * 1024))
                        << " MB against a " << (budget / (1024 * 1024)) << " MB budget, ending with " << ssBC7EvictVerdictName(last) << LL_ENDL;
}
// </SS:Nexii>
