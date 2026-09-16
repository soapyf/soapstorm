/**
 * @file ssbc7store.h
 * @brief Squeeze BC7 sidecar store - append-only index plus capped data segments, see doc/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_BC7STORE_H
#define SS_BC7STORE_H

#include "llsingleton.h"
#include "lluuid.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// BC7 is derived data: the J2C texture cache remains the source of truth, so anything here can be wiped at any time and the only cost is re-encoding. The store is append-only, every record and blob is checksummed, and recovery is a truncate to the last valid record.
//
// FULL-RES-ONLY: a record exists only for a texture encoded at discard 0 with a complete mip chain. Because the payload is stored smallest-mip-first, serving any coarser discard is a prefix read - which is why the pick mask lives after the payload rather than before it.

constexpr U32 SSBC7_IDX_MAGIC        = 0x37434253; // 'SBC7' little-endian
constexpr U32 SSBC7_SEG_MAGIC        = 0x47534253; // 'SBSG'
constexpr U32 SSBC7_BLOB_MAGIC       = 0x42374253; // 'SB7B'
// <SS:Nexii> Squeeze adaptive quality - version 2 adds SSBC7Record::mQuality and the matching byte in the blob header. It is bumped ONCE for that, and the intention is that it never moves for quality again: the whole point of putting the profile in the record is that a store may hold a mixture, so a session that changes profile costs an upgrade pass rather than a wipe.
constexpr U32 SSBC7_FORMAT_VERSION   = 2;          // wipe-never-migrate, exactly as the region cache does

// How many distinct encode profiles a record's quality byte may name. Mirrors SSBC7_QUALITY_COUNT in indra/llimage/ssbc7encoder.h, duplicated rather than included for the same reason SSBC7_MAX_MIPS is: this half of the store reaches for nothing above it, which is what lets the offline harness compile it alone. A value outside the range is CLAMPED on read and never rejected - a record whose pixels are perfectly good must not be thrown away, and taking the whole tail of the index with it, over a byte that only steers a background upgrade pass.
constexpr U32 SSBC7_QUALITY_LEVELS   = 3;
// </SS:Nexii>
constexpr U32 SSBC7_HEADER_SIZE      = 64;
constexpr U32 SSBC7_RECORD_SIZE      = 64;
constexpr U32 SSBC7_BLOB_HEADER_SIZE = 96;
constexpr U32 SSBC7_MAX_MIPS         = 6;          // MAX_DISCARD_LEVEL is 5, so six levels is a hard ceiling
constexpr U64 SSBC7_SEGMENT_CAP      = 512ull * 1024 * 1024;
constexpr U32 SSBC7_ALIGN            = 16;         // every blob starts on a 16 byte boundary

// <SS:Nexii> Squeeze eviction - SSBC7_SEGMENT_CAP above is a FORMAT CEILING and must never move: deserializeRecord rejects any record whose blob ends past it and loadIndex stops at the first rejected record, so lowering it would silently discard the whole tail of an index written by an older build rather than rejecting one record. The cap the writer actually rolls over at is a runtime value derived from the user's budget, which is what gives segment-granular reclaim enough buckets to be selective at the 512 MB low end of the slider.
constexpr U64 SSBC7_SEGMENT_CAP_MIN     = 16ull * 1024 * 1024;
constexpr U32 SSBC7_SEGMENT_BUCKETS     = 24;      // how many segments the budget is divided into, which is the precision of one kill
constexpr U64 SSBC7_EVICT_BATCH         = 64ull * 1024 * 1024;   // the doc's "roughly 64 MB batches", used as the amount of cold tail a pass aims to have marked before it picks a victim
constexpr U32 SSBC7_EVICT_HOT_SECONDS   = 120;     // a hard recency floor that does not depend on mLastUseDay's one-day resolution: anything referenced this recently is never counted as cold
constexpr U32 SSBC7_EVICT_HOT_PERCENT   = 25;      // a sealed segment with more than this fraction of hot bytes is excluded, unless the hard cap forces the issue
constexpr U32 SSBC7_EVICT_COOLDOWN_SECS = 60;      // a moving watermark must not be able to cause back-to-back kills
constexpr U32 SSBC7_EVICT_STARTUP_PASSES = 64;     // bounds what a budget the user lowered by an order of magnitude between sessions can cost at login
constexpr U32 SSBC7_EVICT_DROPPED_CAP    = 65536;  // how many dropped uuids are remembered for the re-encode metric before it becomes a lower bound rather than a count
constexpr U32 SSBC7_EVICT_REWANT_CAP     = 256;    // only the currently-referenced subset of a kill goes back on the want list, and only this many per tick - handing the promotion engine every dropped uuid would send it to re-fetch precisely the textures just judged cold
constexpr U32 SSBC7_EVICT_TICK_SECONDS   = 60;     // how often the main thread does its maintenance pass, which is both the reference sweep and the only thing that ever posts an eviction
// </SS:Nexii>

enum ESSBC7Format : U8
{
    SSBC7_FMT_BC7_UNORM = 1,
    // Reserved so the format byte never has to be reinterpreted: BC7_SRGB, BC6H, BC5, BC1.
};

enum ESSBC7Flags : U16
{
    SSBC7_FLAG_ALPHA_IS_MASK = 1 << 0,  // alpha is effectively binary, which the renderer treats differently from smooth alpha
    SSBC7_FLAG_FULLY_OPAQUE  = 1 << 1,
    SSBC7_FLAG_TOMBSTONE     = 1 << 2,  // this uuid is deleted; a later record always supersedes an earlier one
    SSBC7_FLAG_HAS_PICKMASK  = 1 << 3,
};

// One 64 byte index record. Fixed width so the index is a flat array and record i lives at SSBC7_HEADER_SIZE + i * SSBC7_RECORD_SIZE.
struct SSBC7Record
{
    SSBC7Record();

    LLUUID  mUUID;
    U64     mBlobOffset;        // within the segment, always a multiple of SSBC7_ALIGN
    U32     mBlobSize;          // header + payload + pick mask, excluding the alignment pad
    U32     mPickMaskBytes;     // a U16 would overflow at 2048 squared
    U32     mDataCRC;           // over the whole blob including its own header
    U16     mSegment;
    U16     mWidth;             // full resolution, power of two
    U16     mHeight;
    U16     mLastUseDay;        // days since the epoch - a drop-first priority signal, never an expiry
    U16     mFlags;             // ESSBC7Flags
    U8      mBaseDiscard;       // always 0 while the store is full-res-only
    U8      mMipCount;
    U8      mFormat;            // ESSBC7Format
    U8      mSrcComponents;     // components of the ORIGINAL image - BC7 is always four channel, and without this every opaque texture lands in the alpha pool
    // <SS:Nexii> Squeeze adaptive quality - SSBC7Quality, SERIALISED, and the field this whole feature is built around. With the profile here rather than in the encoder version, the store may hold records made at three different profiles at once, so the controller can drop to a cheaper one under load without invalidating everything already written - and a record made in a hurry becomes something the idle upgrade pass can re-encode better later rather than something that is simply wrong.
    U8      mQuality;
    // </SS:Nexii>
    U32     mRecordCRC;

    // <SS:Nexii> Squeeze eviction - IN MEMORY ONLY, never serialised. mLastUseDay is a U16 of days, so on a cache filled in one session every record shares a single value and a day-only sort is arbitrary order; the tick breaks ties inside today and the second is a hard recency floor. Losing both to a crash costs ordering quality and nothing else, which is exactly what a drop-priority signal is allowed to lose.
    U32     mTouchTick;
    U32     mTouchSecond;
    // </SS:Nexii>

    bool isTombstone() const { return (mFlags & SSBC7_FLAG_TOMBSTONE) != 0; }
};

// The 96 byte header at the start of every blob. Self-describing on purpose: if the index is ever lost, the segments alone are enough to rebuild it.
struct SSBC7BlobHeader
{
    SSBC7BlobHeader();

    LLUUID  mUUID;
    U16     mWidth;
    U16     mHeight;
    U8      mBaseDiscard;
    U8      mMipCount;
    U8      mFormat;
    U8      mSrcComponents;
    U16     mFlags;
    U8      mQuality;                  // <SS:Nexii/> Squeeze adaptive quality - the same byte as the record's, in the blob too, because the header's stated purpose is that the segments alone are enough to rebuild a lost index and a rebuild that forgot the profile would send the upgrade pass over everything
    U32     mPickMaskBytes;
    U32     mPayloadBytes;
    U32     mPickMaskCRC;
    U32     mMipCRC[SSBC7_MAX_MIPS];   // indexed by STORE order, so entry 0 is the smallest mip - this is what makes a prefix read verifiable
    U32     mPayloadCRC;
    U32     mEncoderVersion;
};

// Everything needed to append one texture. The payload must already be in store order, smallest mip first, because that is the layout the GL upload loop walks backwards from the base level.
struct SSBC7Encoded
{
    SSBC7Encoded();

    LLUUID           mUUID;
    U16              mWidth;
    U16              mHeight;
    U8               mMipCount;
    U8               mSrcComponents;
    U8               mQuality;    // <SS:Nexii/> Squeeze adaptive quality - carried in rather than looked up, so ssBC7BuildBlob stays a pure function of its argument and the offline harness can build a record at any profile without a backend
    U16              mFlags;
    std::vector<U8>  mPayload;    // concatenated mips, smallest first
    std::vector<U8>  mPickMask;   // optional, may be empty
};

// Byte-exact serialisation of one blob, exposed so it can be unit tested without touching the filesystem.
bool ssBC7BuildBlob(const SSBC7Encoded& src, U32 encoder_version, std::vector<U8>& out_blob, SSBC7Record& out_record);
bool ssBC7ParseBlobHeader(const U8* data, size_t size, SSBC7BlobHeader& out);

// <SS:Nexii> Squeeze read path - the prefix read is the entire reason the payload is stored smallest-mip-first, and it needs a header parse that does not insist the whole payload is present. ssBC7ParseBlobHeader keeps its existing contract by calling this and then applying the total-size check itself, so no existing caller changes behaviour and there is still only one place the header layout is written down.
bool ssBC7ParseBlobHeaderOnly(const U8* data, size_t size, SSBC7BlobHeader& out);

// Payload bytes occupied by the first `levels` STORE-ORDER levels, which is exactly what serving discard (mip_count - levels) has to read off disk. Returns zero for inconsistent arguments, which a caller must treat as a refusal rather than as an empty read.
U32 ssBC7PrefixBytes(U32 width, U32 height, U32 mip_count, U32 levels);

// Verifies a TRUNCATED blob carrying only its first `levels` store-order levels, against the per-level checksums the writer records in store order. The expected uuid is required for the same reason ssBC7VerifyBlob requires it: a reclaimed segment id makes a well formed blob for the wrong texture indistinguishable from the right one.
bool ssBC7VerifyBlobPrefix(const U8* data, size_t size, const LLUUID& expect_uuid, U32 levels);
// </SS:Nexii>

// <SS:Nexii> Squeeze eviction - the expected uuid is a REQUIRED parameter rather than an optional extra check. Once a segment id can be reclaimed and handed out again, a stale offset - held by a second instance whose index snapshot predates a kill, or by anything that cached a record across a purge - lands on a blob that is perfectly well formed and passes every checksum it carries, and the only thing that distinguishes it from the blob the caller asked for is its uuid. Making it a parameter is what stops a future fast path from skipping the check by accident.
bool ssBC7VerifyBlob(const U8* data, size_t size, const LLUUID& expect_uuid);
// </SS:Nexii>

// <SS:Nexii> Squeeze eviction - the two clocks a drop-priority signal is stamped from, defined in the IO half. Days is the one field that persists; seconds is the in-memory recency floor and never reaches the disk. Both are wall clock on purpose: a clock that jumps costs eviction ordering and nothing else, which is the whole latitude a priority signal has that a TTL would not.
U16 ssBC7Today();
U32 ssBC7NowSeconds();
// </SS:Nexii>

// Size of one BC7 level in bytes, with the four by four block clamp that a two by two or one by one mip still occupies a whole sixteen byte block.
U32 ssBC7LevelBytes(U32 width, U32 height);

// Total payload size of a full chain, and the byte offset of a given store-order level within that payload.
U32 ssBC7PayloadBytes(U32 width, U32 height, U32 mip_count);
U32 ssBC7LevelOffset(U32 width, U32 height, U32 mip_count, U32 store_index);

// ---------------------------------------------------------------------------
// The on-disk store
// ---------------------------------------------------------------------------

constexpr const char* SSBC7_DIR_NAME = "bc7cache";   // lives INSIDE the J2C texture cache directory, so an older viewer sharing the cache never sees it and a directory-level wipe takes it along
constexpr const char* SSBC7_IDX_NAME = "bc7.idx";
constexpr U32 SSBC7_SEG_HEADER_SIZE  = 64;           // a multiple of SSBC7_ALIGN, so the first blob lands aligned without a pad
constexpr U32 SSBC7_MAX_SEGMENTS     = 4096;         // 512 MB each - far past any plausible cache size, and a bound is what stops a corrupt segment index from being followed anywhere

// <SS:Nexii> Squeeze eviction - why a pass ended the way it did. A bare "return false" makes "eviction is switched off" and "eviction ran and refused every candidate" produce the same silence, which is the exact failure this project has already paid for once, so every exit carries a name that is counted and logged.
enum ESSBC7EvictVerdict
{
    SSBC7_EVICT_RAN = 0,            // a segment was killed; the log line carries how much came back and how much of it was still warm
    SSBC7_EVICT_NOT_NEEDED,         // under budget, which is the normal answer and the whole point of a lax watermark
    SSBC7_EVICT_DISABLED,
    SSBC7_EVICT_READ_ONLY,          // a second instance never evicts, exactly as it never writes
    SSBC7_EVICT_NOT_READY,          // the store never came up, or shutdown has begun
    SSBC7_EVICT_STALE,              // initStore or purgeAll moved underneath the pass; the cursor it was holding is meaningless now
    SSBC7_EVICT_ALREADY_RUNNING,
    SSBC7_EVICT_COOLDOWN,           // too soon after the last kill for a moving watermark to be believed
    SSBC7_EVICT_BUDGET_INVALID,     // SSBC7CacheSize is nonsense, and guessing a budget the user did not ask for is worse than declining
    SSBC7_EVICT_NO_CANDIDATE,       // nothing but the write head exists, so there is no sealed segment to reclaim
    SSBC7_EVICT_ALL_HOT,            // every sealed segment is over the hot bar; the store sits above the watermark and says so, rather than dropping something the user is looking at right now
    SSBC7_EVICT_STILL_REFERENCED,   // the post-condition check found a live record still pointing at the victim, so the file was left alone
    SSBC7_EVICT_INDEX_WRITE_FAILED, // nothing on disk changed
    SSBC7_EVICT_UNLINK_FAILED,      // the records are dead and the file is an orphan for the startup sweep - NEVER rolled back, because un-killing records is where this wedges
    SSBC7_EVICT_COUNT
};

const char* ssBC7EvictVerdictName(ESSBC7EvictVerdict verdict);
// </SS:Nexii>

// Append-only index plus capped data segments, both written by one process and read back at startup in one sequential pass.
//
// TWO LOCKS, NEVER NESTED THE OTHER WAY: mMapMutex guards the in-memory index only and is held for microseconds; mStoreMutex guards every byte of file IO. Multi-megabyte appends must never share a mutex with the lookup path or the main thread stalls behind a disk write.
//
// NOTHING IS EVER TRUNCATED. Recovery after a kill is a logical end computed from the index rather than a truncate call: the next append simply overwrites whatever partial blob was left behind, which needs no platform-specific file-shortening call and cannot itself fail halfway. The only visible cost is a segment file that is briefly longer than its logical contents.
class SSBC7Store : public LLSingleton<SSBC7Store>
{
    LLSINGLETON(SSBC7Store);
    ~SSBC7Store();

public:
    // SSSqueezeEnabled gates the whole tier. While it is off nothing here touches the disk at all.
    static bool enabled();

    // `texture_cache_dir` is LLTextureCache's own directory, passed in rather than recomputed so the two can never disagree about where the sidecar lives. `encoder_version` is stamped into the index header and any mismatch on the next run wipes the tier, which is what keeps the store from ever handing a blob to a different encoder's reader - passed in rather than queried so the store itself stays independent of llimage.
    void initStore(const std::string& texture_cache_dir, U32 encoder_version);

    // Call before the encode pool is closed. Refuses further appends, so a worker that is already mid-encode finds the door shut instead of writing into a store that is about to be torn down.
    void beginShutdown();
    void shutdownStore();

    bool isInitialized() const { return mInitialized; }
    bool isReadOnly() const { return mReadOnly; }

    // Plain-UUID dedupe. Sufficient because full-res-only makes every record immutable per (uuid, encoder_version) - there are no better discards to supersede.
    //
    // <SS:Nexii> Squeeze eviction - NON-CONST because a hit is the reference signal. This is the only place in the tree that learns a stored texture is still wanted: it fires from the encode gate on every full-resolution decode that reaches DONE, which includes J2C cache hits, so stamping the day and a session tick here is what stops the scorer from being plain creation-order FIFO. It costs one store into a record under the map lock this call already takes - no new lock, no IO, no queued work.
    bool hasRecord(const LLUUID& id);
    bool lookup(const LLUUID& id, SSBC7Record& out);
    // </SS:Nexii>

    // Serialises, appends the blob to the current segment, then appends the index record. Blob first, always: an index record pointing at a blob that was never written is a dangling reference, while a blob nothing points at is merely wasted space the next compaction reclaims.
    //
    // <SS:Nexii> Squeeze adaptive quality - `allow_supersede` is how the idle upgrade pass replaces a record with a better encode of the same texture, and it is OFF for every ordinary append so the plain-uuid dedupe is unchanged for the demand path. Superseding is an append of a newer record for the same uuid, exactly as a tombstone is, so loadIndex already resolves it on the way back in - the old blob simply becomes garbage its segment reclaims when it is eventually killed. A supersede that is not strictly an improvement is refused and counted, because re-encoding a texture at the profile it already has spends CPU and disk to change nothing.
    bool append(const SSBC7Encoded& src, U32 encoder_version, bool allow_supersede = false);

    // Up to `max_count` uuids of live records encoded BELOW `target_quality`, worst first so the ugliest records are repaired before the merely imperfect ones. One walk of the index under the map lock, taken only when the machine is otherwise idle. `out_remaining` is the total that still qualify, which is what makes the readout say "1,240 improved, 3,891 still to go" rather than a number with no denominator.
    size_t takeUpgradeCandidates(U8 target_quality, size_t max_count, const std::unordered_set<LLUUID>& skip, std::vector<LLUUID>& out, U32& out_remaining);

    // Live records per quality level, maintained incrementally on every append, supersede, kill and index load, so a readout costs a copy of three counters rather than the index walk the overlay must never provoke.
    void qualityHistogram(U32 out_counts[SSBC7_QUALITY_LEVELS]) const;

    // Records replaced by a better encode of the same texture this session, which is the number that makes the idle upgrade pass visible to a person instead of being a thing the viewer does silently.
    U32 upgradesApplied() const;
    // </SS:Nexii>

    // Hooked into every cache-clear path. A BC7 store is a complete second copy of every texture the user has looked at, so it has to die with the rest of the cache or "clear cache" is a lie.
    void purgeAll();

    // <SS:Nexii> Squeeze eviction - the whole reason lookup() exists. A caller handed a raw (segment, offset) could not tell a reclaimed segment from the one it meant, so the read lives inside the store where the uuid check cannot be skipped, and a record whose blob no longer verifies erases itself so a stale snapshot stops retrying.
    bool readBlob(const LLUUID& id, std::vector<U8>& out_blob, SSBC7Record& out_record);

    // <SS:Nexii> Squeeze read path - reader-pool thread only, blocking IO, no lock held across the read, exactly as readBlob. `levels` counts store-order levels up from the smallest and zero means the whole chain; serving discard d wants (mMipCount - d) of them, so a coarse serve costs a short read instead of a full-resolution one. The returned buffer is header plus that prefix and NOTHING else, which is what makes the pointer arithmetic at the upload site the same in both cases.
    // <SS:Nexii/> Squeeze pick masks - `out_pickmask`, when given and the record carries one, receives the mask from the tail of the blob, checksummed against the header, in the same file open as the prefix. It lives after the payload precisely so the prefix read stays a prefix; the cost of fetching it is one more seek and a read of a few tens of kilobytes at most.
    bool readBlobPrefix(const LLUUID& id, U32 levels, std::vector<U8>& out_blob, SSBC7Record& out_record, std::vector<U8>* out_pickmask = nullptr);
    // </SS:Nexii>

    // Any thread. Marks every uuid still referenced by the running viewer, which is the other half of the heat signal: permanently resident textures - avatar skins, UI, system assets - decode once at login and never again, so a fetch-time-only signal would rank exactly those as the coldest things in the store. Returns the referenced uuids a kill dropped this session, which is the only subset worth re-encoding straight away.
    void touchReferenced(const std::vector<LLUUID>& ids, std::vector<LLUUID>& out_rewant);

    // Encode-pool worker. One pass either kills exactly one segment or explains why it did not. Never called on the main thread except from initStore, where nothing else is running yet.
    ESSBC7EvictVerdict evictPass(bool respect_cooldown);

    // Set from inside appendBlob, which runs under mStoreMutex - hence a flag rather than a post. The once-a-minute tick does the posting, outside every lock.
    void requestEvictionCheck() { mEvictWanted = true; }
    bool evictionWanted() const { return mEvictWanted.load(); }

    // Only a hint to whoever is deciding whether to queue a pass. The pass itself owns the mutual exclusion, so a stale read here costs one wasted work item and never a second concurrent kill.
    bool evictionRunning() const { return mEvictRunning.load(); }

    U64 allocatedBytes() const;     // what the store actually costs on disk, which is what the budget is enforced against
    U64 budgetBytes() const;        // SSBC7CacheSize, in bytes; zero means the setting is unusable
    U64 segmentCap() const { return mSegmentCap; }
    U32 generation() const { return mGeneration.load(); }
    // </SS:Nexii>

    std::string storeDir() const { return mStoreDir; }
    std::string indexPath() const;
    std::string segmentPath(U32 segment) const;

    U64 dataBytesUsed() const { return mDataBytes.load(); }
    // <SS:Nexii> Squeeze eviction - the live texture count and the index append cursor are now different numbers, because a tombstone advances the cursor without adding a texture. They used to be the same variable, which is also the variable the next append writes at, so a batched tombstone write that forgot to advance it would have the next append overwrite the tombstones it had just committed.
    U32 recordCount() const { return mLiveRecords.load(); }
    U32 indexCursor() const { return mRecordCursor.load(); }
    // </SS:Nexii>

    struct Metrics
    {
        // <SS:Nexii> Squeeze eviction - an array of atomics cannot use a member initialiser, so the constructor is what keeps the verdict tally at zero.
        Metrics() { for (U32 i = 0; i < SSBC7_EVICT_COUNT; ++i) mEvictVerdicts[i] = 0; }
        // </SS:Nexii>

        std::atomic<U32> mAppendsOk{0};
        std::atomic<U32> mAppendsFailed{0};
        std::atomic<U32> mAppendsDuplicate{0};
        std::atomic<U32> mRecordsLoaded{0};
        std::atomic<U32> mRecordsRejected{0};   // index entries dropped at startup because their checksum or their geometry did not survive the last session
        std::atomic<U64> mBytesWritten{0};

        // <SS:Nexii> Squeeze eviction - mReEncodedAfterEvict is the metric that decides whether this strategy is working: re-encode IS the copy phase here, so if re-encoded bytes ever run past roughly a sixth of bytes written, copy-forward or an explicit tenured generation is worth revisiting. Everything else is here so a log line can tell "off" from "ran and declined".
        std::atomic<U32> mAppendsOverBudget{0};
        std::atomic<U32> mEvictPasses{0};
        std::atomic<U32> mSegmentsKilled{0};
        std::atomic<U64> mBytesReclaimed{0};
        std::atomic<U32> mRecordsDropped{0};
        std::atomic<U32> mHotRecordsDropped{0};
        std::atomic<U32> mReEncodedAfterEvict{0};
        std::atomic<U32> mOrphansSwept{0};
        std::atomic<U32> mUnlinkFailures{0};
        std::atomic<U32> mIndexCompactions{0};
        std::atomic<U32> mEvictVerdicts[SSBC7_EVICT_COUNT];
        // </SS:Nexii>
    };
    Metrics& metrics() { return mMetrics; }
    std::string metricsString() const;

    // Exposed for the offline harness: a record is 64 bytes with its own checksum, so index recovery can be tested without a filesystem.
    static void serializeRecord(const SSBC7Record& rec, U8* out);
    static bool deserializeRecord(const U8* in, SSBC7Record& out);

    // The container headers, likewise pure. Both version numbers are passed in rather than looked up, which is what lets ssbc7store.cpp - and therefore the offline test - stay clear of every viewer global.
    static void buildIndexHeader(U32 encoder_version, U32 j2c_version, std::vector<U8>& out);
    static bool parseIndexHeader(const U8* data, U32 encoder_version, U32 j2c_version);
    static void buildSegmentHeader(U32 segment, std::vector<U8>& out);

private:
    bool loadIndex();
    bool writeIndexHeader();
    bool appendIndexRecord(const SSBC7Record& rec);
    bool appendBlob(const std::vector<U8>& blob, U16& out_segment, U64& out_offset);
    bool ensureSegment(U32 segment);

    // <SS:Nexii> Squeeze eviction - one open, one write, one flush for the whole batch. appendIndexRecord does a full open/seek/write/flush/close per record, and one kill is hundreds to thousands of tombstones, so calling it in a loop would turn a 64 byte durability point into thousands of them.
    bool appendIndexRecords(const std::vector<SSBC7Record>& recs);

    // Called under mStoreMutex. Hands out the lowest reclaimed id before growing the head, because SSBC7_MAX_SEGMENTS is consumed at the rate the store is WRITTEN rather than the rate it grows - at the observed write rate monotonic ids would exhaust the space in about a day of exploring, after which appendBlob refuses for the rest of the session.
    bool allocateSegment(U32& out_segment);

    // The body of one pass. Split out so evictPass can own the in-progress flag with a single acquire and a single release rather than threading a clear through a dozen early returns, which is exactly where a "declined" that never unlocks would hide.
    ESSBC7EvictVerdict evictPassLocked(bool respect_cooldown);

    // Everything below runs at startup only, before the store is published: no readers, no writers, no lock ordering question, and the one moment where a budget the user just lowered gets honoured without pushing IO into a live session.
    void sweepOrphanSegments();
    void compactIndex();
    void startupTrim();
    bool killSegmentFile(U32 segment, U32 expect_generation);   // the ONLY delete in the whole tier, and it builds its own path; takes mStoreMutex and re-checks the generation before unlinking
    // </SS:Nexii>

    // Atomic because the encode workers read all three on every append while the main thread is the only writer. They are flags, not state machines - no worker ever needs two of them to agree with each other.
    std::atomic<bool>               mInitialized{false};
    std::atomic<bool>               mShuttingDown{false};
    std::atomic<bool>               mReadOnly{false};   // a second instance shares the cache directory and is a read-only snapshot, exactly as LLTextureCache and LLVOCache are
    std::string                     mStoreDir;
    U32                             mCurrentSegment;
    U64                             mSegmentEnd;    // logical end of mCurrentSegment, which is where the next blob goes
    U32                             mEncoderVersion;
    std::atomic<U64>                mDataBytes{0};

    // <SS:Nexii> Squeeze eviction - state guarded by mStoreMutex unless marked otherwise. Nothing here is ever persisted: the frontiers, the free list and the byte totals are all derived from one index scan plus one directory listing, so there is no second file to fall out of step with bc7.idx and therefore no torn cross-file state to reason about.
    U64                             mSegmentCap;        // runtime rollover threshold, always <= SSBC7_SEGMENT_CAP
    U32                             mSegmentHigh;       // highest id ever handed out this session
    U32                             mDyingSegment;      // SSBC7_MAX_SEGMENTS when none, so the allocator cannot hand out an id a kill is still working through
    std::vector<U64>                mSegFrontier;       // logical end per segment, zero when the segment does not exist
    std::vector<U32>                mFreeSegments;      // kept sorted; ids reclaimed this session plus the holes derived at startup
    std::atomic<U64>                mAllocBytes{0};     // sum of the frontiers, which is the number the budget is enforced against - mDataBytes excludes alignment pads and drops the instant a record is tombstoned, while the disk does not
    std::atomic<U32>                mRecordCursor{0};   // where the next index record goes
    std::atomic<U32>                mLiveRecords{0};
    std::atomic<U32>                mGeneration{0};     // bumped by initStore and purgeAll; re-checked at every eviction step, because purgeAll deletes the directory on the main thread and can interleave with a pass
    std::atomic<bool>               mEvictWanted{false};
    std::atomic<bool>               mEvictRunning{false};
    std::atomic<U32>                mSessionTick{0};
    std::atomic<U32>                mLastEvictSecond{0};

    std::unordered_map<U16, std::vector<LLUUID>> mSegMembers;   // guarded by mMapMutex; scoring only, never the authority for what a kill erases
    std::unordered_set<LLUUID>      mDroppedUUIDs;      // guarded by mMapMutex; capped, and exists purely so re-encode-after-eviction can be MEASURED - re-encode is this design's copy phase, and if it costs more than a small fraction of bytes written then copy-forward or an explicit tenured generation is the better answer after all
    // </SS:Nexii>

    mutable std::mutex              mMapMutex;
    std::unordered_map<LLUUID, SSBC7Record> mIndex;
    // <SS:Nexii> Squeeze adaptive quality - guarded by mMapMutex alongside mIndex, and derived from it rather than persisted: every path that adds, replaces or drops a record adjusts these in the same critical section, so they cannot drift from the map they describe without the map itself being wrong.
    U32                             mQualityCounts[SSBC7_QUALITY_LEVELS] = {0, 0, 0};
    U32                             mUpgradesApplied = 0;
    // </SS:Nexii>

    mutable std::mutex              mStoreMutex;
    Metrics                         mMetrics;
};

#endif // SS_BC7STORE_H
