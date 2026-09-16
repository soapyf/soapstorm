/**
 * @file ssbc7storeio.cpp
 * @brief Squeeze BC7 sidecar store, IO half - directories, handles, recovery and the disk budget, see doc/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssbc7store.h"

// Split from ssbc7store.cpp deliberately: everything that decides what the bytes MEAN lives there and depends on nothing, so it can be tested offline; everything that decides where they GO lives here and pulls in the viewer's directories, settings and second-instance rules.
//
// The eviction POLICY - which segment dies and why - lives next door again in ssbc7storeevict.cpp. This file owns every byte that reaches the disk, including the ones eviction writes, so that there is exactly one place where a path is built and exactly one place where a file is deleted.

#include "llappviewer.h"
#include "lldir.h"
#include "lldiriterator.h"
#include "llfile.h"
#include "llviewercontrol.h"
#include "ssstratabudget.h"   // <SS:Nexii/> budgetBytes is a share of CacheSize decided by the arbiter, not a number of this store's own
#include "ssserial.h"         // <SS:Nexii/> Squeeze pick masks - crc32Of, to check the mask read off the blob's tail against the header

#include <cstdio>
#include <cstring>
#include <ctime>

namespace
{
    bool ssBC7WriteAt(LLFILE* f, U64 offset, const void* data, size_t bytes)
    {
        // Every offset in this store is bounded by SSBC7_SEGMENT_CAP for segments and by record count times 64 for the index, so a 32 bit fseek is always enough; the guard is here so that stops being an assumption if either bound ever moves.
        if (offset > 0x7FFFFFFFull) return false;
        if (fseek(f, (long)offset, SEEK_SET) != 0) return false;
        return fwrite(data, 1, bytes, f) == bytes;
    }

    LLFILE* ssBC7OpenRW(const std::string& path)
    {
        LLFILE* f = LLFile::fopen(path, "r+b");
        if (!f) f = LLFile::fopen(path, "w+b");
        return f;
    }
}

// Days since the unix epoch. A drop-first priority signal only - it is never read as an expiry, so a clock that jumps backwards costs eviction ordering and nothing else.
U16 ssBC7Today()
{
    return (U16)llmin((U64)(time(NULL) / 86400), (U64)0xFFFFu);
}

// <SS:Nexii> Squeeze eviction - the recency floor's clock. Wall seconds rather than a frame timer because the eviction pass runs on a worker that has no notion of frames, and a clock jump costs ordering quality and nothing else.
U32 ssBC7NowSeconds()
{
    return (U32)((U64)time(NULL) & 0xFFFFFFFFull);
}
// </SS:Nexii>

SSBC7Store::SSBC7Store()
:   mCurrentSegment(0),
    mSegmentEnd(SSBC7_SEG_HEADER_SIZE),
    mEncoderVersion(0),
    // <SS:Nexii> Squeeze eviction
    mSegmentCap(SSBC7_SEGMENT_CAP),
    mSegmentHigh(0),
    mDyingSegment(SSBC7_MAX_SEGMENTS)
    // </SS:Nexii>
{
    // <SS:Nexii> Squeeze eviction - 32 KB of frontiers, allocated once, so the budget can be enforced against what the segments actually occupy rather than against the sum of live blob sizes.
    mSegFrontier.assign((size_t)SSBC7_MAX_SEGMENTS, (U64)0);
    // </SS:Nexii>
}

SSBC7Store::~SSBC7Store()
{
}

bool SSBC7Store::enabled()
{
    static LLCachedControl<bool> squeeze_enabled(gSavedSettings, "SSSqueezeEnabled", false);
    return squeeze_enabled;
}

std::string SSBC7Store::indexPath() const
{
    return gDirUtilp->add(mStoreDir, SSBC7_IDX_NAME);
}

std::string SSBC7Store::segmentPath(U32 segment) const
{
    return gDirUtilp->add(mStoreDir, llformat("bc7_%03u.dat", segment));
}

// <SS:Nexii> The BC7 budget is a SHARE OF THE TOTAL CACHE, not a number of its own. CacheSize is what the user actually thinks of as "how much disk may the viewer use", and a tier with a separate absolute setting quietly spends beyond it - which is how this store reached 2.2 GB on top of a 16 GB texture cache without anything being wrong by its own reckoning.
//
// This restores what Linden originally intended and Firestorm departed from: the commented-out DiskCachePercentOfTotal / texture_cache_percent pair in llappviewer.cpp, and its surviving comment "the maximum size of this cache is defined as a percentage of the total cache size - the 'CacheSize' pref - for all caches".
//
// SSBC7CacheSize survives as an absolute override for anyone who wants to pin the tier regardless of the total; a percent of zero selects it. Read live rather than latched, so moving either slider mid-session starts draining at the next tick rather than at the next restart.
//
// STAGE 2, doc/strata.md: the four lines of arithmetic that used to live here moved to the arbiter in ssstratabudget.cpp, which is now the ONLY place that decides how CacheSize is divided - the same percent, the same clamp to 90 and the same "a zero share selects SSBC7CacheSize" escape hatch, reproduced there byte for byte. This function's contract is unchanged: read live, zero means the setting is unusable and the eviction pass declines with BUDGET_INVALID rather than guessing a budget the user did not ask for. Until SSStrataBudgetEnforce is turned on the arbiter hands back exactly what this function used to compute.
U64 SSBC7Store::budgetBytes() const
{
    return ssBudgetTierBytes(SSBUDGET_TIER_BC7);
}

U64 SSBC7Store::allocatedBytes() const
{
    // The sum of the segment frontiers plus the index file, which is what the user paid for on disk. mDataBytes is the sum of LIVE blob sizes: it excludes alignment pads and it drops the instant a record is tombstoned, while the bytes on disk do not move until the unlink, so a budget built on it would let the store sit permanently over the slider.
    return mAllocBytes.load() + (U64)SSBC7_HEADER_SIZE + (U64)mRecordCursor.load() * SSBC7_RECORD_SIZE;
}
// </SS:Nexii>

void SSBC7Store::initStore(const std::string& texture_cache_dir, U32 encoder_version)
{
    if (mInitialized || !enabled()) return;
    if (texture_cache_dir.empty())
    {
        LL_WARNS("Squeeze") << "BC7 store not started: the texture cache directory is not set yet" << LL_ENDL;
        return;
    }

    // <SS:Nexii> Squeeze eviction - initStore can legitimately run more than once, so anything an eviction pass might still be holding from a previous incarnation is invalidated here rather than being allowed to write into the store this call is building.
    ++mGeneration;
    // </SS:Nexii>

    mStoreDir = gDirUtilp->add(texture_cache_dir, SSBC7_DIR_NAME);
    mEncoderVersion = encoder_version;

    // A second instance shares the cache directory and is a read-only snapshot, exactly the posture LLTextureCache and LLVOCache take. It reads what is already there and never encodes.
    mReadOnly = LLAppViewer::instance() && LLAppViewer::instance()->isSecondInstance();

    // <SS:Nexii> Squeeze eviction - the rollover threshold is derived from the budget so that a kill is always a small slice of it. At the 512 MB slider minimum a 512 MB segment would be the ENTIRE store in one bucket, which segment-granular reclaim cannot work with at all. SSBC7_SEGMENT_CAP stays where it is as the format ceiling.
    {
        const U64 budget = budgetBytes();
        mSegmentCap = budget ? llclamp(budget / SSBC7_SEGMENT_BUCKETS, SSBC7_SEGMENT_CAP_MIN, SSBC7_SEGMENT_CAP)
                             : SSBC7_SEGMENT_CAP;
    }
    // </SS:Nexii>

    if (!mReadOnly)
    {
        LLFile::mkdir(texture_cache_dir);
        LLFile::mkdir(mStoreDir);
    }

    mInitialized = true;
    mShuttingDown = false;

    if (!loadIndex())
    {
        // Version mismatch or an unreadable header. The whole tier is derived data, so it is wiped rather than migrated - the only cost is re-encoding, and that is exactly the precedent the texture cache version bump sets.
        LL_INFOS("Squeeze") << "BC7 store at " << mStoreDir << " is from another version or unreadable, wiping" << LL_ENDL;
        purgeAll();
    }
    else if (!mReadOnly)
    {
        // <SS:Nexii> Squeeze eviction - the whole maintenance sequence runs HERE, before the store is published: no readers, no writers, no in-flight appends and no lock ordering question, and it is the one moment where a budget the user lowered between sessions gets honoured. The order matters - sweep first so the accounting is honest, trim second so the tombstones exist, compact last so the index shrinks past both.
        sweepOrphanSegments();
        startupTrim();
        compactIndex();
        // </SS:Nexii>
    }

    LL_INFOS("Squeeze") << "BC7 store at " << mStoreDir
                        << (mReadOnly ? " (read only, second instance)" : "")
                        << " holding " << recordCount() << " textures in "
                        << (mDataBytes.load() / (1024 * 1024)) << " MB"
                        // <SS:Nexii> Squeeze eviction - the numbers that decide whether anything gets dropped, printed at every startup so "eviction never ran" is never a guess.
                        << ", occupying " << (allocatedBytes() / (1024 * 1024)) << " MB of a "
                        << (budgetBytes() / (1024 * 1024)) << " MB budget in "
                        << (mSegmentCap / (1024 * 1024)) << " MB segments"
                        // </SS:Nexii>
                        << LL_ENDL;
}

void SSBC7Store::beginShutdown()
{
    mShuttingDown = true;
}

void SSBC7Store::shutdownStore()
{
    // No drain loop here on purpose: the encode pool owns every in-flight append and is closed before this runs, so by the time we arrive there is nothing left that could still be writing. See ssbc7encodequeue.cpp for why abandoning beats draining at quit.
    mShuttingDown = true;
    mInitialized = false;

    std::lock_guard<std::mutex> lock(mMapMutex);
    mIndex.clear();
    // <SS:Nexii> Squeeze eviction
    mSegMembers.clear();
    mDroppedUUIDs.clear();
    // </SS:Nexii>
}

bool SSBC7Store::hasRecord(const LLUUID& id)
{
    std::lock_guard<std::mutex> lock(mMapMutex);
    auto it = mIndex.find(id);
    if (it == mIndex.end() || it->second.isTombstone()) return false;

    // <SS:Nexii> Squeeze eviction - THE reference signal. This fires from the encode gate on every full-resolution decode that reaches DONE, which includes plain J2C cache hits, so it is the one place the store learns that a texture it already holds is still being looked at. Without it mLastUseDay would be written once at append and never again, and the scorer would be creation-order FIFO - which on a first session drops the home region first, because the home region is the thing that was encoded first.
    it->second.mLastUseDay  = ssBC7Today();
    it->second.mTouchTick   = ++mSessionTick;
    it->second.mTouchSecond = ssBC7NowSeconds();
    // </SS:Nexii>
    return true;
}

bool SSBC7Store::lookup(const LLUUID& id, SSBC7Record& out)
{
    std::lock_guard<std::mutex> lock(mMapMutex);
    auto it = mIndex.find(id);
    if (it == mIndex.end() || it->second.isTombstone()) return false;

    // <SS:Nexii> Squeeze eviction - a read is a reference, and stamping it here as well as in hasRecord is what keeps the two entry points from disagreeing about what "used" means.
    it->second.mLastUseDay  = ssBC7Today();
    it->second.mTouchTick   = ++mSessionTick;
    it->second.mTouchSecond = ssBC7NowSeconds();
    // </SS:Nexii>

    out = it->second;
    return true;
}

// <SS:Nexii> Squeeze eviction - the read path lives inside the store so the uuid check cannot be bypassed by a caller who was handed a raw offset. A second instance holds an index snapshot that the primary's kills and segment reuse can invalidate at any moment, and the only thing separating "the texture you asked for" from "a perfectly valid blob for something else" is the uuid in the blob header.
bool SSBC7Store::readBlob(const LLUUID& id, std::vector<U8>& out_blob, SSBC7Record& out_record)
{
    out_blob.clear();

    if (!mInitialized || mShuttingDown) return false;
    if (!lookup(id, out_record)) return false;
    if (out_record.mBlobSize < SSBC7_BLOB_HEADER_SIZE) return false;

    // No lock at all across the file IO: the map lock was released by lookup, and the store lock guards appends rather than reads. A kill running concurrently can only remove the record from the map, which costs this read nothing - and if the file goes away underneath us the open or the read fails and we fall back exactly as if there had never been a record.
    LLFILE* f = LLFile::fopen(segmentPath(out_record.mSegment), "rb");
    if (!f)
    {
        LL_DEBUGS("Squeeze") << "BC7 segment " << out_record.mSegment << " for " << id << " could not be opened, falling back to J2C" << LL_ENDL;
        return false;
    }

    out_blob.resize(out_record.mBlobSize);
    const bool read_ok = fseek(f, (long)out_record.mBlobOffset, SEEK_SET) == 0
                      && fread(out_blob.data(), 1, out_record.mBlobSize, f) == out_record.mBlobSize;
    LLFile::close(f);

    if (!read_ok || !ssBC7VerifyBlob(out_blob.data(), out_blob.size(), id))
    {
        // The record is wrong about the disk. Drop it from this process's map so a stale snapshot stops asking the same question forever, and let the ordinary J2C path serve the texture. The index file is deliberately not touched: a read-only second instance must never write, and the primary rebuilds the truth at its next startup anyway.
        {
            std::lock_guard<std::mutex> lock(mMapMutex);
            if (mIndex.erase(id)) { if (mLiveRecords.load()) --mLiveRecords; }
        }
        out_blob.clear();
        LL_WARNS("Squeeze") << "BC7 blob for " << id << " in segment " << out_record.mSegment
                            << " did not verify against the requested uuid, dropping the record and using J2C" << LL_ENDL;
        return false;
    }
    return true;
}
// </SS:Nexii>

// <SS:Nexii> Squeeze read path - the short read the smallest-mip-first layout was designed for. Everything about the failure handling is deliberately identical to readBlob: a record that is wrong about the disk is erased from this process's map so a stale snapshot stops asking forever, the index file is never touched because a read-only second instance must never write, and the caller falls back to J2C.
bool SSBC7Store::readBlobPrefix(const LLUUID& id, U32 levels, std::vector<U8>& out_blob, SSBC7Record& out_record, std::vector<U8>* out_pickmask)
{
    out_blob.clear();
    if (out_pickmask) out_pickmask->clear();

    // <SS:Nexii> Every exit below says which one it was. These used to be six bare returns, and because the pump maps any false onto the single verdict DECLINE_READ, "this texture has no record", "its record was evicted while we were reading" and "the index disagrees with itself about its geometry" arrived at the log as the same number - which is precisely the failure this tier's own verdict enum exists to prevent, and which this project has already paid for twice elsewhere.
    //
    // Debug level rather than info: a miss is the ordinary state of a warming cache and would drown the log, but a reason that is only in the binary is not a reason at all.
    if (!mInitialized || mShuttingDown)
    {
        LL_DEBUGS("Squeeze") << "BC7 read for " << id << " declined: the store is not up" << LL_ENDL;
        return false;
    }
    if (!lookup(id, out_record))
    {
        LL_DEBUGS("Squeeze") << "BC7 read for " << id << " declined: no record, either never encoded or evicted since the probe" << LL_ENDL;
        return false;
    }
    if (out_record.mBlobSize < SSBC7_BLOB_HEADER_SIZE)
    {
        LL_WARNS("Squeeze") << "BC7 read for " << id << " declined: the index claims a blob of " << out_record.mBlobSize
                            << " bytes, smaller than a header - the index is inconsistent" << LL_ENDL;
        return false;
    }
    if (out_record.mMipCount == 0 || out_record.mMipCount > SSBC7_MAX_MIPS)
    {
        LL_WARNS("Squeeze") << "BC7 read for " << id << " declined: the index claims " << (U32)out_record.mMipCount
                            << " mips, which is impossible - the index is inconsistent" << LL_ENDL;
        return false;
    }

    if (levels == 0 || levels > out_record.mMipCount) levels = out_record.mMipCount;

    // Sized from the INDEX record rather than from the blob header, because the read length has to be known before the file is opened - that is what makes this one seek and one read rather than a header read followed by a second one.
    const U32 prefix = ssBC7PrefixBytes(out_record.mWidth, out_record.mHeight, out_record.mMipCount, levels);
    if (prefix == 0)
    {
        LL_WARNS("Squeeze") << "BC7 read for " << id << " declined: " << out_record.mWidth << "x" << out_record.mHeight
                            << " over " << (U32)out_record.mMipCount << " mips gives a zero length prefix" << LL_ENDL;
        return false;
    }

    const U64 want = (U64)SSBC7_BLOB_HEADER_SIZE + (U64)prefix;
    if (want > (U64)out_record.mBlobSize)
    {
        LL_WARNS("Squeeze") << "BC7 read for " << id << " declined: the prefix wants " << want
                            << " bytes but the index says the blob is only " << out_record.mBlobSize << LL_ENDL;
        return false;
    }

    LLFILE* f = LLFile::fopen(segmentPath(out_record.mSegment), "rb");
    if (!f)
    {
        LL_DEBUGS("Squeeze") << "BC7 segment " << out_record.mSegment << " for " << id << " could not be opened, falling back to J2C" << LL_ENDL;
        return false;
    }

    out_blob.resize((size_t)want);
    const bool read_ok = fseek(f, (long)out_record.mBlobOffset, SEEK_SET) == 0
                      && fread(out_blob.data(), 1, (size_t)want, f) == (size_t)want;
    const bool prefix_ok = read_ok && ssBC7VerifyBlobPrefix(out_blob.data(), out_blob.size(), id, levels);

    // <SS:Nexii> Squeeze pick masks - the mask sits after the WHOLE payload, so it is a second seek in the same open rather than part of the prefix. Its failure is not the prefix's failure: a mask that does not read or does not checksum is dropped and the texture serves without one - which the serve gate then declines for an alpha texture, exactly as it would a record that never had a mask - while the payload, which verified on its own CRCs, stays good.
    if (prefix_ok && out_pickmask && (out_record.mFlags & SSBC7_FLAG_HAS_PICKMASK) && out_record.mPickMaskBytes)
    {
        SSBC7BlobHeader hdr;
        bool mask_ok = ssBC7ParseBlobHeaderOnly(out_blob.data(), out_blob.size(), hdr)
                    && hdr.mPickMaskBytes == out_record.mPickMaskBytes
                    && (U64)SSBC7_BLOB_HEADER_SIZE + hdr.mPayloadBytes + hdr.mPickMaskBytes <= (U64)out_record.mBlobSize;
        if (mask_ok)
        {
            out_pickmask->resize(hdr.mPickMaskBytes);
            const U64 mask_at = out_record.mBlobOffset + SSBC7_BLOB_HEADER_SIZE + hdr.mPayloadBytes;
            mask_ok = fseek(f, (long)mask_at, SEEK_SET) == 0
                   && fread(out_pickmask->data(), 1, hdr.mPickMaskBytes, f) == (size_t)hdr.mPickMaskBytes
                   && ssserial::crc32Of(out_pickmask->data(), hdr.mPickMaskBytes) == hdr.mPickMaskCRC;
        }
        if (!mask_ok)
        {
            out_pickmask->clear();
            LL_WARNS("Squeeze") << "BC7 pick mask for " << id << " in segment " << out_record.mSegment
                                << " did not read or verify, the texture will serve without per-texel picking" << LL_ENDL;
        }
    }
    // </SS:Nexii>
    LLFile::close(f);

    if (!prefix_ok)
    {
        {
            std::lock_guard<std::mutex> lock(mMapMutex);
            if (mIndex.erase(id)) { if (mLiveRecords.load()) --mLiveRecords; }
        }
        out_blob.clear();
        LL_WARNS("Squeeze") << "BC7 prefix read of " << id << " in segment " << out_record.mSegment
                            << " (" << levels << " of " << (U32)out_record.mMipCount << " levels, " << want
                            << " bytes) did not verify, dropping the record and using J2C" << LL_ENDL;
        return false;
    }
    return true;
}
// </SS:Nexii>

bool SSBC7Store::loadIndex()
{
    const std::string path = indexPath();

    // Every session-scoped counter is reset here rather than assumed clean. initStore can legitimately run more than once - the tier can be switched on mid-session, and a mid-session cache clear puts it right back through here - and a record count carried over from a previous store would place the next append past a slot nobody ever wrote, leaving a hole of zeros that fails its checksum and truncates the whole index on the next startup.
    mCurrentSegment = 0;
    mSegmentEnd = SSBC7_SEG_HEADER_SIZE;
    mRecordCursor = 0;
    mLiveRecords = 0;
    mDataBytes = 0;
    // <SS:Nexii> Squeeze eviction - the derived state goes with it. None of this is ever persisted, so a stale frontier or free list carried across an initStore would be the only way the store could ever disagree with its own index.
    mAllocBytes = 0;
    mSegmentHigh = 0;
    mDyingSegment = SSBC7_MAX_SEGMENTS;
    mSegFrontier.assign((size_t)SSBC7_MAX_SEGMENTS, (U64)0);
    mFreeSegments.clear();
    // </SS:Nexii>
    {
        std::lock_guard<std::mutex> lock(mMapMutex);
        for (U32 q = 0; q < SSBC7_QUALITY_LEVELS; ++q) mQualityCounts[q] = 0;   // <SS:Nexii/> Squeeze adaptive quality
        mIndex.clear();
        mSegMembers.clear();
        mDroppedUUIDs.clear();
    }

    LLFILE* f = LLFile::fopen(path, "rb");
    if (!f)
    {
        // No index at all is the normal first-run case, not a failure. Return true so a fresh header is written rather than the directory being wiped for no reason.
        if (!mReadOnly) writeIndexHeader();
        return true;
    }

    U8 header[SSBC7_HEADER_SIZE];
    const bool read_header = fread(header, 1, SSBC7_HEADER_SIZE, f) == SSBC7_HEADER_SIZE;
    if (!read_header)
    {
        LLFile::close(f);
        return false;
    }

    if (!parseIndexHeader(header, mEncoderVersion, (U32)LLAppViewer::getTextureCacheVersion()))
    {
        LLFile::close(f);
        return false;
    }

    // One sequential pass. The first record that fails its own checksum ends the index: everything after a torn append is unreachable by construction, and the next append overwrites it.
    std::unordered_map<LLUUID, SSBC7Record> index;
    U32 count = 0;
    U8 buf[SSBC7_RECORD_SIZE];

    while (fread(buf, 1, SSBC7_RECORD_SIZE, f) == SSBC7_RECORD_SIZE)
    {
        SSBC7Record rec;
        if (!deserializeRecord(buf, rec))
        {
            ++mMetrics.mRecordsRejected;
            break;
        }

        // A later record for the same uuid always supersedes an earlier one, which is what makes the file append-only rather than rewritten.
        if (rec.isTombstone()) index.erase(rec.mUUID);
        else                   index[rec.mUUID] = rec;

        ++count;
    }
    LLFile::close(f);

    // <SS:Nexii> Squeeze eviction - SECOND PASS, over the SURVIVORS ONLY. Deriving the segment frontiers inside the loop above was a latent bug the moment anything could be dropped: it accumulated the ends of superseded and tombstoned records too, so after a kill the next startup would park the write head at the dead segment's old logical end and the first append would re-grow the file that had just been deleted, as a hole. Taken over the final map instead, a fully dead segment derives a frontier of nothing at all, which is what makes id reuse work without an allocator and gives free tail reclaim when the coldest records happen to sit at a segment's end.
    std::vector<U64> segment_end((size_t)SSBC7_MAX_SEGMENTS, (U64)0);
    std::unordered_map<U16, std::vector<LLUUID>> members;
    U64 data_bytes = 0;

    for (const auto& entry : index)
    {
        const SSBC7Record& rec = entry.second;
        const U64 end = rec.mBlobOffset + rec.mBlobSize;
        if (end > segment_end[rec.mSegment]) segment_end[rec.mSegment] = end;
        data_bytes += rec.mBlobSize;
        members[rec.mSegment].push_back(entry.first);
    }

    U64 alloc = 0;
    for (U32 s = 0; s < SSBC7_MAX_SEGMENTS; ++s)
    {
        if (segment_end[s] == 0) continue;
        mSegFrontier[s] = llmax(segment_end[s], (U64)SSBC7_SEG_HEADER_SIZE);
        alloc += mSegFrontier[s];
        mCurrentSegment = s;   // the write head is the highest segment still holding a LIVE record
        mSegmentHigh = s;
    }
    mSegmentEnd = mSegFrontier[mCurrentSegment] ? mSegFrontier[mCurrentSegment] : (U64)SSBC7_SEG_HEADER_SIZE;
    mAllocBytes = alloc;

    // The free list is derived, never persisted: every id below the head that no live record points into is reusable, and the orphan sweep is what makes sure any file still sitting on one of them is gone.
    for (U32 s = 0; s < mSegmentHigh; ++s)
    {
        if (mSegFrontier[s] == 0) mFreeSegments.push_back(s);
    }
    // </SS:Nexii>

    mRecordCursor = count;
    mLiveRecords = (U32)index.size();
    mDataBytes = data_bytes;
    mMetrics.mRecordsLoaded = (U32)index.size();

    // <SS:Nexii/> Squeeze adaptive quality - the histogram is derived from the survivors in the same pass that derives the frontiers, so a session that starts with a store full of hurried records knows on the first tick how much the upgrade pass owes rather than after a scan nobody scheduled.
    for (U32 q = 0; q < SSBC7_QUALITY_LEVELS; ++q) mQualityCounts[q] = 0;
    for (const auto& entry : index)
    {
        const U8 q = entry.second.mQuality;
        if (q < SSBC7_QUALITY_LEVELS) ++mQualityCounts[q];
    }

    {
        std::lock_guard<std::mutex> lock(mMapMutex);
        mIndex.swap(index);
        mSegMembers.swap(members);
    }
    return true;
}

bool SSBC7Store::writeIndexHeader()
{
    if (mReadOnly) return false;

    std::vector<U8> header;
    buildIndexHeader(mEncoderVersion, (U32)LLAppViewer::getTextureCacheVersion(), header);

    LLFILE* f = ssBC7OpenRW(indexPath());
    if (!f) return false;
    const bool ok = ssBC7WriteAt(f, 0, header.data(), header.size());
    LLFile::close(f);
    return ok;
}

bool SSBC7Store::ensureSegment(U32 segment)
{
    // Caller holds mStoreMutex.
    const std::string path = segmentPath(segment);
    const bool exists = gDirUtilp->fileExists(path);

    // <SS:Nexii> Squeeze eviction - a reused id whose file survived a failed unlink is NOT empty, and the old code returned early on any existing file without rewriting the header. The frontier is the store's own opinion of how much of this segment is real, so anything at or below the header offset is treated as a fresh segment and gets its header rewritten. Stale blobs past the new write point remain, and are harmless only because every read checks the blob's uuid against the one it asked for.
    if (exists && mSegFrontier[segment] > (U64)SSBC7_SEG_HEADER_SIZE) return true;
    // </SS:Nexii>

    // The directory can vanish under a running session: a mid-session cache clear deletes it, and the next encode to land has to find a home rather than silently failing every append for the rest of the session.
    LLFile::mkdir(mStoreDir);

    std::vector<U8> header;
    buildSegmentHeader(segment, header);

    LLFILE* f = ssBC7OpenRW(path);
    if (!f) return false;
    const bool ok = ssBC7WriteAt(f, 0, header.data(), header.size());
    LLFile::close(f);

    // <SS:Nexii> Squeeze eviction - a segment that exists costs its header even before the first blob lands, and the budget is enforced against what exists.
    if (ok && mSegFrontier[segment] < (U64)SSBC7_SEG_HEADER_SIZE)
    {
        mAllocBytes += (U64)SSBC7_SEG_HEADER_SIZE - mSegFrontier[segment];
        mSegFrontier[segment] = SSBC7_SEG_HEADER_SIZE;
    }
    // </SS:Nexii>
    return ok;
}

bool SSBC7Store::appendBlob(const std::vector<U8>& blob, U16& out_segment, U64& out_offset)
{
    // Caller holds mStoreMutex. Every blob starts on a SSBC7_ALIGN boundary so a future memory-mapped or direct-IO reader never has to care about alignment.
    U64 offset = (mSegmentEnd + (SSBC7_ALIGN - 1)) & ~(U64)(SSBC7_ALIGN - 1);

    // <SS:Nexii> Squeeze eviction - the runtime cap, not the format ceiling. See the note above SSBC7_SEGMENT_CAP_MIN for why the two must stay different numbers.
    if (offset + blob.size() > mSegmentCap)
    {
        U32 next = 0;
        if (!allocateSegment(next))
        {
            LL_WARNS("Squeeze") << "BC7 store is out of segments, stopping writes for this session" << LL_ENDL;
            return false;
        }
        mCurrentSegment = next;
        mSegmentEnd = SSBC7_SEG_HEADER_SIZE;
        offset = SSBC7_SEG_HEADER_SIZE;

        // This is the only moment the store's disk footprint grows by a whole bucket, so it is the natural place to ask for a check - but it is a FLAG rather than a post, because we are inside mStoreMutex here and posting work while holding the lock that work will want is how a deadlock gets written. The once-a-minute tick does the posting, outside every lock.
        requestEvictionCheck();
    }
    // </SS:Nexii>

    if (!ensureSegment(mCurrentSegment)) return false;

    LLFILE* f = ssBC7OpenRW(segmentPath(mCurrentSegment));
    if (!f) return false;

    // Handles are opened and closed per append rather than held: an open handle defeats the directory-rename trick the texture cache purge relies on, and one open plus one close per encoded texture is nothing next to the encode that produced it.
    const bool ok = ssBC7WriteAt(f, offset, blob.data(), blob.size()) && fflush(f) == 0;
    LLFile::close(f);
    if (!ok) return false;

    mSegmentEnd = offset + blob.size();
    // <SS:Nexii> Squeeze eviction
    if (mSegmentEnd > mSegFrontier[mCurrentSegment])
    {
        mAllocBytes += mSegmentEnd - mSegFrontier[mCurrentSegment];
        mSegFrontier[mCurrentSegment] = mSegmentEnd;
    }
    // </SS:Nexii>
    out_segment = (U16)mCurrentSegment;
    out_offset  = offset;
    return true;
}

bool SSBC7Store::appendIndexRecord(const SSBC7Record& rec)
{
    // Caller holds mStoreMutex.
    U8 buf[SSBC7_RECORD_SIZE];
    serializeRecord(rec, buf);

    LLFILE* f = ssBC7OpenRW(indexPath());
    if (!f) return false;

    const U64 offset = (U64)SSBC7_HEADER_SIZE + (U64)mRecordCursor.load() * SSBC7_RECORD_SIZE;
    const bool ok = ssBC7WriteAt(f, offset, buf, SSBC7_RECORD_SIZE) && fflush(f) == 0;
    LLFile::close(f);
    if (!ok) return false;

    ++mRecordCursor;
    return true;
}

// <SS:Nexii> Squeeze eviction - ONE open, ONE contiguous write, ONE flush for the whole batch. A kill tombstones every record in a segment, which is hundreds to thousands of them, and appendIndexRecord above does a full open/seek/write/flush/close each time; calling it in a loop would turn a single durability point into thousands of them and hold mStoreMutex for the duration. The cursor advances by exactly N under the same lock, because it is also the offset the next append writes at.
bool SSBC7Store::appendIndexRecords(const std::vector<SSBC7Record>& recs)
{
    // Caller holds mStoreMutex.
    if (recs.empty()) return true;

    std::vector<U8> buf(recs.size() * SSBC7_RECORD_SIZE);
    for (size_t i = 0; i < recs.size(); ++i)
    {
        serializeRecord(recs[i], buf.data() + i * SSBC7_RECORD_SIZE);
    }

    LLFILE* f = ssBC7OpenRW(indexPath());
    if (!f) return false;

    const U64 offset = (U64)SSBC7_HEADER_SIZE + (U64)mRecordCursor.load() * SSBC7_RECORD_SIZE;
    // No fsync. Every crash ordering collapses to the same read-side uuid and checksum check, so the extra barrier would buy nothing but SSD wear.
    const bool ok = ssBC7WriteAt(f, offset, buf.data(), buf.size()) && fflush(f) == 0;
    LLFile::close(f);
    if (!ok) return false;

    mRecordCursor += (U32)recs.size();
    return true;
}

// THE ONLY DELETE IN THE WHOLE TIER. It takes a segment id rather than a path so no caller can ever hand it one, and it refuses outright unless the directory it is about to touch is the store's own.
bool SSBC7Store::killSegmentFile(U32 segment, U32 expect_generation)
{
    // <SS:Nexii> The generation is re-checked HERE, under the lock, and the unlink happens while it is still held. Checking it only after the unlink - which is what this did first - leaves a window that destroys a live store: an eviction pass descheduled just before the unlink, a purgeAll on the main thread that deletes the directory and recreates it, encode workers that immediately refill a brand new bc7_000.dat, and then the stalled pass wakes up and unlinks THAT file. The pass would then see the changed generation and report that it had done nothing, while the fresh index still pointed into a file that no longer exists.
    //
    // Holding mStoreMutex across the unlink is the point rather than a compromise. It is the mutex that serialises file operations, and purgeAll takes it to delete the directory, so the two orderings are the only two that exist: either this deletes a file the purge was about to delete anyway, or it wakes to a changed generation and refuses. An unlink can cost tens of milliseconds on a large NTFS file, but eviction runs about once a minute and only ever blocks the encode pool, never the main thread.
    std::lock_guard<std::mutex> lock(mStoreMutex);

    if (mGeneration.load() != expect_generation)
    {
        LL_INFOS("Squeeze") << "BC7 eviction abandoned the unlink of segment " << segment << ": the store was purged and recreated underneath it" << LL_ENDL;
        return false;
    }
    // </SS:Nexii>

    if (mReadOnly) return false;
    if (mStoreDir.empty()) return false;
    if (segment >= SSBC7_MAX_SEGMENTS) return false;

    // The last component of the store directory must be the store's own name. This is the guard that stands between a mis-set cache path and deleting somebody's documents.
    const std::string& delim = gDirUtilp->getDirDelimiter();
    const size_t cut = mStoreDir.find_last_of(delim);
    const std::string leaf = (cut == std::string::npos) ? mStoreDir : mStoreDir.substr(cut + 1);
    if (leaf != std::string(SSBC7_DIR_NAME))
    {
        LL_WARNS("Squeeze") << "BC7 store refused to delete inside " << mStoreDir << ": that is not a " << SSBC7_DIR_NAME << " directory" << LL_ENDL;
        return false;
    }

    const std::string path = segmentPath(segment);
    if (LLFile::isdir(path)) return false;   // a directory wearing a segment's name is not ours to remove
    if (!gDirUtilp->fileExists(path)) return true;   // already gone counts as reclaimed

    return LLFile::remove(path, ENOENT) == 0;
}

// Deletes any segment file no live record points into. Runs at startup only, where it is also the cleanup for a crash between a kill's index write and its unlink.
void SSBC7Store::sweepOrphanSegments()
{
    if (mReadOnly || mStoreDir.empty()) return;

    std::vector<U32> orphans;
    U64 on_disk = 0;
    U64 derived = 0;

    {
        LLDirIterator iter(mStoreDir, "bc7_*.dat");
        std::string name;
        while (iter.next(name))
        {
            // ROUND-TRIP THE NAME. Parse the id out, rebuild the canonical filename from it, and require an exact string match - anything unexpected fails to match and is skipped rather than guessed at. Four digits are legal because "bc7_%03u.dat" stops padding past 999.
            if (name.size() < 11) continue;   // "bc7_000.dat" is eleven characters, and a filter set one higher than that rejects every canonically named segment there is
            if (name.compare(0, 4, "bc7_") != 0) continue;

            const size_t dot = name.find('.', 4);
            if (dot == std::string::npos || name.compare(dot, std::string::npos, ".dat") != 0) continue;

            const std::string digits = name.substr(4, dot - 4);
            if (digits.empty() || digits.size() > 5) continue;
            if (digits.find_first_not_of("0123456789") != std::string::npos) continue;

            const U32 id = (U32)strtoul(digits.c_str(), NULL, 10);
            if (id >= SSBC7_MAX_SEGMENTS) continue;
            if (llformat("bc7_%03u.dat", id) != name) continue;

            const std::string path = gDirUtilp->add(mStoreDir, name);
            if (LLFile::isdir(path)) continue;

            llstat st;
            const bool statted = LLFile::stat(path, &st, ENOENT) == 0;

            if (mSegFrontier[id] == 0 && id != mCurrentSegment)
            {
                orphans.push_back(id);
                continue;
            }

            if (statted) on_disk += (U64)st.st_size;
            derived += mSegFrontier[id];
        }
    }

    for (U32 id : orphans)
    {
        if (killSegmentFile(id, mGeneration.load()))
        {
            ++mMetrics.mOrphansSwept;
            LL_INFOS("Squeeze") << "BC7 store swept orphan segment " << id << ", which no live record pointed into" << LL_ENDL;
        }
        else
        {
            ++mMetrics.mUnlinkFailures;
            LL_WARNS("Squeeze") << "BC7 store could not delete orphan segment " << id << " (errno " << errno << "), leaving it for the next startup" << LL_ENDL;
        }
    }

    // The accounting is DERIVED, so it is worth saying out loud when it disagrees with the disk rather than quietly trusting it. Drift is expected and small - torn tails past the last valid index record are exactly this - but a large number here means the frontier arithmetic is wrong.
    if (on_disk != derived)
    {
        LL_INFOS("Squeeze") << "BC7 store accounting drift: segments hold " << (on_disk / 1024) << " KB on disk against "
                            << (derived / 1024) << " KB derived from the index; the difference is torn tails past the last valid record" << LL_ENDL;
    }
}

// Rewrites bc7.idx with the live records only. Startup only, before the store is published, so there is no reader to race and no writer to serialise against. A crash before the rename leaves the old index, a crash after leaves the new one, and nothing anywhere references a record by its position.
void SSBC7Store::compactIndex()
{
    if (mReadOnly || mStoreDir.empty()) return;

    const U32 cursor = mRecordCursor.load();
    const U32 live   = mLiveRecords.load();
    if (cursor < 64) return;                 // an index this small is not worth a rewrite
    if (live * 2 > cursor) return;           // less than half dead, so leave it alone

    std::vector<SSBC7Record> keep;
    {
        std::lock_guard<std::mutex> lock(mMapMutex);
        keep.reserve(mIndex.size());
        for (const auto& entry : mIndex)
        {
            // SCRUB WHERE THE BYTES ARE ALREADY IN HAND: a record whose blob would run past the end of the file it claims to live in is dropped here rather than carried forward to fail a read later.
            llstat st;
            if (LLFile::stat(segmentPath(entry.second.mSegment), &st, ENOENT) != 0) continue;
            if ((U64)st.st_size < entry.second.mBlobOffset + entry.second.mBlobSize) continue;
            keep.push_back(entry.second);
        }
    }

    const std::string tmp = indexPath() + ".tmp";

    {
        std::vector<U8> header;
        buildIndexHeader(mEncoderVersion, (U32)LLAppViewer::getTextureCacheVersion(), header);

        std::vector<U8> body(keep.size() * SSBC7_RECORD_SIZE);
        for (size_t i = 0; i < keep.size(); ++i) serializeRecord(keep[i], body.data() + i * SSBC7_RECORD_SIZE);

        LLFILE* f = LLFile::fopen(tmp, "wb");
        if (!f)
        {
            LL_WARNS("Squeeze") << "BC7 index compaction skipped: " << tmp << " could not be created" << LL_ENDL;
            return;
        }
        const bool ok = fwrite(header.data(), 1, header.size(), f) == header.size()
                     && (body.empty() || fwrite(body.data(), 1, body.size(), f) == body.size())
                     && fflush(f) == 0;
        LLFile::close(f);
        if (!ok)
        {
            LLFile::remove(tmp, ENOENT);
            LL_WARNS("Squeeze") << "BC7 index compaction skipped: the replacement index could not be written" << LL_ENDL;
            return;
        }
    }

    if (LLFile::rename(tmp, indexPath()) != 0)
    {
        LLFile::remove(tmp, ENOENT);
        LL_WARNS("Squeeze") << "BC7 index compaction skipped: the replacement index could not be moved into place (errno " << errno << ")" << LL_ENDL;
        return;
    }

    // The map is already correct; only the cursor moves, and it must move to exactly the number of records that were written or the next append lands in the wrong slot.
    const U32 dropped = live - (U32)keep.size();
    mRecordCursor = (U32)keep.size();
    mLiveRecords  = (U32)keep.size();
    ++mMetrics.mIndexCompactions;

    if (dropped)
    {
        std::lock_guard<std::mutex> lock(mMapMutex);
        std::unordered_map<LLUUID, SSBC7Record> rebuilt;
        rebuilt.reserve(keep.size());
        mSegMembers.clear();
        // <SS:Nexii/> Squeeze adaptive quality - recounted rather than adjusted by `dropped`, because compaction is the one path that rebuilds the map wholesale and a delta applied to a map that was just replaced is a guess. It runs once at startup, so the extra walk costs nothing anyone can measure.
        for (U32 q = 0; q < SSBC7_QUALITY_LEVELS; ++q) mQualityCounts[q] = 0;
        for (const SSBC7Record& rec : keep)
        {
            rebuilt[rec.mUUID] = rec;
            mSegMembers[rec.mSegment].push_back(rec.mUUID);
            if (rec.mQuality < SSBC7_QUALITY_LEVELS) ++mQualityCounts[rec.mQuality];
        }
        mIndex.swap(rebuilt);
    }

    LL_INFOS("Squeeze") << "BC7 index compacted from " << cursor << " records to " << keep.size()
                        << (dropped ? llformat(", scrubbing %u whose blobs were missing", dropped) : std::string()) << LL_ENDL;
}
// </SS:Nexii>

bool SSBC7Store::append(const SSBC7Encoded& src, U32 encoder_version, bool allow_supersede)
{
    if (!mInitialized || mShuttingDown || mReadOnly || !enabled())
    {
        ++mMetrics.mAppendsFailed;
        return false;
    }

    // <SS:Nexii> Squeeze eviction - THE HARD CAP. A design that can decline to evict must not also be allowed to grow forever, so past the budget plus two segments of slack the store stops accepting work and says why. Two segments is the slack eviction itself needs: one for the write head and one for the bucket a rollover has just opened.
    {
        const U64 budget = budgetBytes();
        if (budget && allocatedBytes() > budget + 2 * mSegmentCap)
        {
            ++mMetrics.mAppendsOverBudget;
            requestEvictionCheck();
            LL_DEBUGS("Squeeze") << "BC7 store declined " << src.mUUID << ": over budget and eviction has not caught up" << LL_ENDL;
            return false;
        }
    }
    // </SS:Nexii>

    // <SS:Nexii> Squeeze adaptive quality - records used to be immutable per (uuid, encoder_version), which was true while every record in a store shared one profile. Now that they do not, a second encode of the same texture is worth keeping when and only when it is at a STRICTLY BETTER profile, and the demand path never asks for that - only the idle upgrade pass does.
    //
    // The comparison is on the profile ordinal, which is ordered by cost and therefore by quality, so "better" is a plain greater-than and there is no table to keep in step. Refusing an equal or worse re-encode is not a nicety: without it a mistimed upgrade pass would rewrite the whole store to change nothing, and every rewrite leaves the old blob behind as garbage until its segment is killed.
    bool superseding = false;
    {
        SSBC7Record existing;
        std::lock_guard<std::mutex> lock(mMapMutex);
        auto it = mIndex.find(src.mUUID);
        const bool have = (it != mIndex.end() && !it->second.isTombstone());
        if (have)
        {
            if (!allow_supersede || src.mQuality <= it->second.mQuality)
            {
                ++mMetrics.mAppendsDuplicate;
                return false;
            }
            superseding = true;
        }
    }
    // </SS:Nexii>

    std::vector<U8> blob;
    SSBC7Record rec;
    if (!ssBC7BuildBlob(src, encoder_version, blob, rec))
    {
        ++mMetrics.mAppendsFailed;
        LL_WARNS("Squeeze") << "BC7 blob for " << src.mUUID << " failed to serialise at "
                            << src.mWidth << "x" << src.mHeight << " with " << (S32)src.mMipCount << " mips" << LL_ENDL;
        return false;
    }

    rec.mLastUseDay = ssBC7Today();
    // <SS:Nexii> Squeeze eviction - writing a record IS a reference, and a brand new record that looked untouched would sort below everything the session had merely read.
    rec.mTouchTick   = ++mSessionTick;
    rec.mTouchSecond = ssBC7NowSeconds();
    // </SS:Nexii>

    {
        std::lock_guard<std::mutex> lock(mStoreMutex);

        U16 segment = 0;
        U64 offset = 0;
        // Blob first, then the index record that points at it. A dangling index entry would be followed by a reader; an orphaned blob is only wasted bytes.
        if (!appendBlob(blob, segment, offset))
        {
            ++mMetrics.mAppendsFailed;
            LL_WARNS("Squeeze") << "BC7 blob write failed for " << src.mUUID << ", the texture keeps using the ordinary J2C path" << LL_ENDL;
            return false;
        }

        rec.mSegment    = segment;
        rec.mBlobOffset = offset;

        if (!appendIndexRecord(rec))
        {
            ++mMetrics.mAppendsFailed;
            LL_WARNS("Squeeze") << "BC7 index write failed for " << src.mUUID << ", the blob is orphaned and will be reclaimed" << LL_ENDL;
            return false;
        }

        mMetrics.mBytesWritten += blob.size();
        mDataBytes += blob.size();
    }

    {
        std::lock_guard<std::mutex> lock(mMapMutex);
        // <SS:Nexii> Squeeze adaptive quality - the record being replaced is read back HERE rather than trusted from the check above, because the map lock was dropped in between for the multi-megabyte write and a kill may have taken the old record in the meantime. If it is gone this is simply a fresh record, which is the correct outcome and not an error.
        auto prev = mIndex.find(rec.mUUID);
        const bool replacing = (prev != mIndex.end() && !prev->second.isTombstone());
        if (replacing)
        {
            const U8 old_q = prev->second.mQuality;
            if (old_q < SSBC7_QUALITY_LEVELS && mQualityCounts[old_q]) --mQualityCounts[old_q];
            // The old blob is not erased and its file is not shortened: it becomes garbage its segment reclaims when that segment is eventually killed, exactly as a torn tail does. mDataBytes tracks LIVE bytes, so it loses the old size and gains the new one, while mAllocBytes - the number the budget is enforced against - correctly keeps counting both until the unlink.
            mDataBytes = (mDataBytes.load() > prev->second.mBlobSize) ? (mDataBytes.load() - prev->second.mBlobSize) : 0;
            ++mUpgradesApplied;
        }
        else
        {
            ++mLiveRecords;
        }
        if (rec.mQuality < SSBC7_QUALITY_LEVELS) ++mQualityCounts[rec.mQuality];
        // </SS:Nexii>

        mIndex[rec.mUUID] = rec;
        // <SS:Nexii> Squeeze eviction - the per-segment side index is what lets the scorer weigh a segment without walking the whole map, and mDroppedUUIDs is how re-encode-after-eviction gets measured: a texture that comes back is the copy phase working, and if it comes back too often the strategy is wrong.
        mSegMembers[rec.mSegment].push_back(rec.mUUID);
        if (mDroppedUUIDs.erase(rec.mUUID)) ++mMetrics.mReEncodedAfterEvict;
        // </SS:Nexii>
    }

    // <SS:Nexii/> Squeeze adaptive quality - logged at DEBUG rather than INFO because a busy idle hour is thousands of these; the pass itself reports its totals at INFO, which is the level at which a person wants to read this.
    if (superseding)
    {
        LL_DEBUGS("Squeeze") << "BC7 upgraded " << src.mUUID << " to quality " << (U32)src.mQuality
                             << ", the old blob in segment " << rec.mSegment << " is now garbage its segment will reclaim" << LL_ENDL;
    }

    ++mMetrics.mAppendsOk;
    return true;
}

// <SS:Nexii> Squeeze adaptive quality - everything the idle upgrade pass and the overlay need from the store, and nothing more. Both are cheap by construction: the histogram is a counter copy, and the candidate walk is the one place that touches every record, which is why it only ever runs when the machine has nothing else to do.

size_t SSBC7Store::takeUpgradeCandidates(U8 target_quality, size_t max_count, const std::unordered_set<LLUUID>& skip, std::vector<LLUUID>& out, U32& out_remaining)
{
    out.clear();
    out_remaining = 0;

    if (!mInitialized || mShuttingDown || mReadOnly || max_count == 0) return 0;

    // WORST FIRST, and it matters: FAST is the profile with the 11 dB blind spot on multi-hue blocks, so a record at FAST is visibly wrong in a way a BALANCED one is not. Repairing those before the merely imperfect ones is what makes a short idle window worth having.
    std::vector<LLUUID> by_quality[SSBC7_QUALITY_LEVELS];

    {
        std::lock_guard<std::mutex> lock(mMapMutex);
        for (const auto& entry : mIndex)
        {
            const SSBC7Record& rec = entry.second;
            if (rec.isTombstone()) continue;
            if (rec.mQuality >= target_quality || rec.mQuality >= SSBC7_QUALITY_LEVELS) continue;

            ++out_remaining;

            // Already tried and failed this session - usually because the J2C is no longer on this disk. Counted in the remaining total anyway, because it IS still below the target and a readout that hid it would claim the pass had finished when it had merely given up.
            if (skip.find(entry.first) != skip.end()) continue;

            if (by_quality[rec.mQuality].size() < max_count) by_quality[rec.mQuality].push_back(entry.first);
        }
    }

    for (U32 q = 0; q < SSBC7_QUALITY_LEVELS && out.size() < max_count; ++q)
    {
        for (const LLUUID& id : by_quality[q])
        {
            if (out.size() >= max_count) break;
            out.push_back(id);
        }
    }
    return out.size();
}

void SSBC7Store::qualityHistogram(U32 out_counts[SSBC7_QUALITY_LEVELS]) const
{
    std::lock_guard<std::mutex> lock(mMapMutex);
    for (U32 q = 0; q < SSBC7_QUALITY_LEVELS; ++q) out_counts[q] = mQualityCounts[q];
}

U32 SSBC7Store::upgradesApplied() const
{
    std::lock_guard<std::mutex> lock(mMapMutex);
    return mUpgradesApplied;
}
// </SS:Nexii>

void SSBC7Store::purgeAll()
{
    // A second instance must never delete the primary's cache out from under it, exactly as LLVOCache::removeCache and LLTextureCache::purgeAllTextures refuse in read-only mode.
    if (mReadOnly) return;
    if (mStoreDir.empty()) return;

    // <SS:Nexii> Squeeze eviction - bumped BEFORE the directory goes, so an eviction pass already in flight fails its next generation check and abandons rather than writing a stale cursor into the brand new index this call is about to create.
    ++mGeneration;
    // </SS:Nexii>

    {
        std::lock_guard<std::mutex> lock(mStoreMutex);
        gDirUtilp->deleteDirAndContents(mStoreDir);
        mCurrentSegment = 0;
        mSegmentEnd = SSBC7_SEG_HEADER_SIZE;
        mRecordCursor = 0;
        mLiveRecords = 0;
        mDataBytes = 0;
        // <SS:Nexii> Squeeze eviction
        mAllocBytes = 0;
        mSegmentHigh = 0;
        mDyingSegment = SSBC7_MAX_SEGMENTS;
        mSegFrontier.assign((size_t)SSBC7_MAX_SEGMENTS, (U64)0);
        mFreeSegments.clear();
        // </SS:Nexii>
    }

    {
        std::lock_guard<std::mutex> lock(mMapMutex);
        for (U32 q = 0; q < SSBC7_QUALITY_LEVELS; ++q) mQualityCounts[q] = 0;   // <SS:Nexii/> Squeeze adaptive quality
        mIndex.clear();
        // <SS:Nexii> Squeeze eviction
        mSegMembers.clear();
        mDroppedUUIDs.clear();
        // </SS:Nexii>
    }

    // Recreated immediately rather than left missing, so an encode that lands one millisecond after a mid-session cache clear writes into a store with a valid header instead of manufacturing a headerless index that the next startup would only have to wipe again.
    if (mInitialized && !mShuttingDown)
    {
        LLFile::mkdir(mStoreDir);
        writeIndexHeader();
    }

    LL_INFOS("Squeeze") << "BC7 store wiped at " << mStoreDir << LL_ENDL;
}

std::string SSBC7Store::metricsString() const
{
    // <SS:Nexii> Squeeze eviction - the verdict tally is printed alongside the counters for the same reason the encode queue prints its own: without it a log line cannot tell "eviction is off" from "eviction ran and declined everything", and this project has already paid for that once.
    std::string verdicts;
    for (U32 i = 0; i < SSBC7_EVICT_COUNT; ++i)
    {
        const U32 n = mMetrics.mEvictVerdicts[i].load();
        if (!n) continue;
        if (!verdicts.empty()) verdicts += ", ";
        verdicts += llformat("%s %u", ssBC7EvictVerdictName((ESSBC7EvictVerdict)i), n);
    }
    if (verdicts.empty()) verdicts = "no pass has run";

    return llformat("BC7 store %u textures, %llu MB live, %llu of %llu MB on disk in %llu MB segments | index %u records | appends ok %u dup %u failed %u over budget %u | loaded %u rejected %u | eviction: %s | killed %u segments, %llu MB back, %u records dropped (%u still warm), re-encoded %u, orphans %u, unlink failures %u, compactions %u",
                    mLiveRecords.load(),
                    (unsigned long long)(mDataBytes.load() / (1024 * 1024)),
                    (unsigned long long)(allocatedBytes() / (1024 * 1024)),
                    (unsigned long long)(budgetBytes() / (1024 * 1024)),
                    (unsigned long long)(mSegmentCap / (1024 * 1024)),
                    mRecordCursor.load(),
                    mMetrics.mAppendsOk.load(),
                    mMetrics.mAppendsDuplicate.load(),
                    mMetrics.mAppendsFailed.load(),
                    mMetrics.mAppendsOverBudget.load(),
                    mMetrics.mRecordsLoaded.load(),
                    mMetrics.mRecordsRejected.load(),
                    verdicts.c_str(),
                    mMetrics.mSegmentsKilled.load(),
                    (unsigned long long)(mMetrics.mBytesReclaimed.load() / (1024 * 1024)),
                    mMetrics.mRecordsDropped.load(),
                    mMetrics.mHotRecordsDropped.load(),
                    mMetrics.mReEncodedAfterEvict.load(),
                    mMetrics.mOrphansSwept.load(),
                    mMetrics.mUnlinkFailures.load(),
                    mMetrics.mIndexCompactions.load());
    // </SS:Nexii>
}
