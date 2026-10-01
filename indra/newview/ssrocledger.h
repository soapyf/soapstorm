/**
 * @file ssrocledger.h
 * @brief Region Object Cache Phase 2a: the object ledger and the promotion decision - see doc/region_object_cache.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_ROCLEDGER_H
#define SS_ROCLEDGER_H

#include "llsingleton.h"
#include "lluuid.h"
#include "v3math.h"
#include "ssroccache.h"

#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class LLDataPackerBinaryBuffer;
class LLViewerRegion;
class LLVOCacheEntry;

// Phase 2a of the Region Object Cache: record what the simulator mentions, accumulate the tenure ledger across visits, and decide at region exit which records are worth rezzing later. Nothing here rezzes, injects or writes to the .slc - the whole phase is invisible except for the .roc file and one log line per region.
//
// Two recording hooks feed this, and BOTH are needed. LLViewerRegion::cacheFullUpdate carries the DP blob and covers first sightings and mutations, but on a warm .slc revisit the simulator sends ObjectUpdateCached probes carrying only local id, CRC and update flags (llviewerobjectlist.cpp:849-851) and cacheFullUpdate is never entered for an unchanged object - so LLViewerRegion::probeCache's CRC-match branch is the only thing that ever mentions most of a familiar region.

// ESSROCBlockedBy and the promotion verdict itself now live in ssroccache.h, beside the record they are written onto: the decision is a pure function of one record and one set of thresholds, and keeping it here made it reachable only by a build of the whole viewer. The ledger owns WHEN it runs, what the region contributes to it and what is done with the answer.

// Everything the ledger needs out of a cached object update, read offline from the byte-identical blob at fixed offsets. The fixed prefix runs 0..83 and every field below lives inside it, so no variable-length walk is needed to record an object.
struct SSROCBlobFacts
{
    SSROCBlobFacts();

    LLUUID      mFullID;
    LLUUID      mOwnerID;
    U32         mLocalID;
    U32         mCRC;
    U32         mSpecialCode;
    U32         mParentLocalID;   // 0 when this is a linkset root
    LLVector3   mPos;             // region-local for a root, PARENT-RELATIVE for a child (llvocache.cpp:701-706)
    LLVector3   mScale;
    U8          mPCode;
    U8          mState;           // attachment id when PCode is a volume, species byte on grass (llvograss.cpp:94)
};

// Parse the fixed prefix. Returns false on a short or empty blob rather than reporting zeroes as facts.
bool ssROCParseBlob(const U8* blob, S32 len, SSROCBlobFacts& out);

// Hard disqualifiers, evaluated BEFORE any score so a bonus can never resurrect a moving object.
bool ssROCIsDisqualified(const SSROCBlobFacts& facts, U32 update_flags, S32 blob_len);

// Mine asset UUIDs out of a promoted record's blob for the [MANIFEST] section. Sculpt maps, spotlight textures and GLTF material ids only - diffuse TextureEntry mining is deliberately out of Phase 2a, see the comment on the implementation.
void ssROCMineAssets(const std::vector<U8>& blob, std::vector<LLUUID>& out);

// The two Stage A hooks, called from llviewerregion.cpp. Free functions rather than members so the stock file needs one include and one line, and so the whole feature costs a single predictable branch when it is off.
void ssROCNoteCacheUpdate(LLViewerRegion* regionp, U32 local_id, U32 crc, U32 flags, LLDataPackerBinaryBuffer& dp);
void ssROCNoteCacheProbe(LLViewerRegion* regionp, U32 local_id, U32 crc, U32 flags, LLVOCacheEntry* entry);

class SSROCLedger : public LLSingleton<SSROCLedger>
{
    LLSINGLETON(SSROCLedger);
    ~SSROCLedger();

public:
    // LLWorld::addRegion, via SSROCAuxMgr. Starts tracking and connects the agent-region clock on first use.
    void onRegionAdded(LLViewerRegion* regionp);

    // The aux manager's async load completion, so the ledger starts from the record set already on disk instead of a blank slate.
    void onRegionFileLoaded(U64 handle, SSROCFilePtr file);

    // LLViewerRegion::unpackRegionHandshake, via SSROCAuxMgr. The ledger wants the CacheID for a different reason than the ghost manager does: the file stores ONE CacheID for a whole record set, so a visit whose id changed leaves the file carrying the new id beside records still keyed to the old one, and only the records the simulator re-mentioned that visit are actually current. Without marking the rest, the visit AFTER a restart reads as mode A and would paint remembered objects at ids that belong to somebody else.
    void onRegionHandshake(LLViewerRegion* regionp, const LLUUID& cache_id);

    // LLWorld::removeRegion, via SSROCAuxMgr, before anything is torn down. Runs the promotion pass and merges the ledger into the outgoing file. Returns true when it contributed records, so the aux manager knows the save is worth queueing even if terrain capture refused.
    bool onRegionRemoved(LLViewerRegion* regionp, SSROCRegionFile& file);

    // gAgent's region-changed signal. Stops the dwell clock on the region being left and starts it on the one being entered.
    void onAgentRegionChanged();

    // gAgent's parcel-changed signal. The only parcel the viewer knows anything about for free is the one the agent is standing on, and only while it is standing there - so the name has to be sampled during the visit and consumed at exit.
    void onAgentParcelChanged();

    void shutdown();

    // Fill a region's protocol object cache from this visit's record set, in place of reading the .slc. `sink` is called once per seedable record, freshest first, and returns true when it actually created an entry: a local id the live stream has already claimed is refused there, which is the live-stream-always-wins rule rather than an error.
    //
    // The ledger is the right source rather than the raw file because it is the only copy that has had the epoch pass applied to it - the file on disk carries the marks from the visit that wrote it, and the marks that matter for THIS visit were decided when the handshake and the disk read had both landed.
    U32 seedObjectCache(U64 handle, U32 max_seed, const std::function<bool(const SSROCRecord&)>& sink, SSROCSeedStats& stats, U8& outcome);

    // Stage A. Called from the two llviewerregion.cpp hooks after the free functions above have cleared the enabled gate.
    void noteSighting(LLViewerRegion* regionp, U32 local_id, U32 crc, U32 flags, const U8* blob, S32 blob_len);

    struct Metrics
    {
        U32 mRegionsTracked  = 0;
        U32 mRegionsSandbox  = 0;
        U32 mRecordsCreated  = 0;
        U32 mSightings       = 0;
        U32 mNoBlob          = 0;
        U32 mPromoted        = 0;
        U32 mOwnerLookups    = 0;
    };
    const Metrics& metrics() const { return mMetrics; }

    // <SS:Nexii> The ledger's word on whether a root is landscape or a mover, for the shape census's DYNAMIC classifier (doc/atmo_magic_navmesh.md 15). True when the ledger has an opinion: out_static is false for anything hard-disqualified (physical, character), true for a promoted record or one confirmed still on at least two region entries. Silence (no record, one entry, or a move on record) is no opinion and the census falls back to watching it settle.
    bool restVerdict(U64 handle, const LLUUID& full_id, bool& out_static) const;
    std::string metricsString() const;

private:
    // Per-region working set for the current visit. The persisted records live here during the visit and are merged back into the file at exit, so nothing the ledger accumulates depends on an LLVOCacheEntry surviving - a nudged object losing its .slc entry does not lose its ROC record.
    struct RegionState
    {
        RegionState();

        U64     mHandle;
        LLUUID  mRegionID;
        bool    mSandbox;             // REGION_FLAGS_SANDBOX seen at any point this visit - records nothing at all
        bool    mSandboxParcelName;   // a parcel the agent stood on was named "sandbox" - denies the immunity credit, never recording
        bool    mLoadResolved;        // the async load callback has fired (with or without a file)
        bool    mHaveHandshake;       // the simulator named its CacheID this visit
        bool    mEpochApplied;        // the stored-id epoch has been decided for this visit, which needs BOTH the handshake and the disk read and can only be done once
        LLUUID  mCacheID;             // the simulator's CacheID this visit
        LLUUID  mFileCacheID;         // the CacheID stored beside the records the file was loaded with
        U32     mStaleMarked;         // records whose stored local id was written off as belonging to a dead epoch
        U32     mVisitSecs;           // seconds the AGENT was present, not seconds the circuit was up

        // <SS:Nexii> Seconds this region was in the world, which is a DIFFERENT question from how long the agent stood in it and is the one the settled-stay gate is actually asking. The design specifies that gate as "the same condition the stock save already uses", and that condition is LLViewerRegion::mRegionTimer at llviewerregion.cpp:902 - region lifetime, not agent presence. Measuring it as dwell meant every region the agent never entered had a stay of zero forever, so on a 2048m-draw-distance fork the sixty-one neighbour regions of a sixty-two region session could never have a single record evaluated no matter how many days they accumulated. Dwell is still what buys VISITS in the tenure currency, where agent presence is exactly the right measure; the two are kept apart rather than conflated.
        F64     mTrackedStart;        // LLTimer::getElapsedSeconds() when the region entered the world
        U32     mTrackedSecs;         // resolved at exit, kept for the log line

        U32     mConfirmed;           // records confirmed at least once this visit
        U32     mCreated;
        U32     mNoBlob;
        U32     mOwnerLookups;
        U32     mPromotedThisVisit;   // records that became promoted during this visit's scoring pass, so the exit path can skip re-mining an unchanged manifest

        // Transient per-record state, kept parallel to mRecords rather than on SSROCRecord so nothing session-scoped ever reaches disk.
        struct Live
        {
            U32 mParentLocalID;
            U8  mNoted;               // confirmed at least once this visit, so entries and days are counted exactly once
        };

        std::vector<SSROCRecord>        mRecords;
        std::vector<Live>               mLive;
        std::unordered_map<LLUUID, U32> mByFullID;
        std::unordered_map<U32, U32>    mByLocalID;
    };

    RegionState* stateFor(U64 handle);
    U32  addRecord(RegionState& rs, const SSROCBlobFacts& facts, U32 flags, const U8* blob, S32 blob_len);
    void confirmRecord(RegionState& rs, U32 index, U32 local_id, U32 crc, U32 flags, U64 now);

    // Owner classification, memoised for the whole process. Moles repeat across the entire grid, so after the first mainland region this answers without issuing a lookup at all.
    enum EOwnerClass : U8 { OWNER_UNKNOWN = 0, OWNER_ORDINARY = 1, OWNER_PUBLIC_WORKS = 2 };
    U8   classifyOwner(RegionState& rs, const LLUUID& owner_id);

    // Decide, once per visit, which stored local ids still name the objects they were stored for. Runs when both the handshake and the disk read have landed, in whichever order they arrive.
    void applyIDEpoch(RegionState& rs);

    void runPromotion(LLViewerRegion* regionp, RegionState& rs);
    F32  scoreRecord(const SSROCRecord& rec) const;
    bool hasImmunity(LLViewerRegion* regionp, const RegionState& rs, const SSROCRecord& rec) const;
    void flushDwell();

    std::map<U64, RegionState>       mRegions;
    std::unordered_map<LLUUID, U8>   mOwnerClass;
    U64                              mDwellHandle;   // the region the agent is currently standing in, or 0
    F64                              mDwellStart;
    bool                             mDwellHooked;
    Metrics                          mMetrics;
};

#endif // SS_ROCLEDGER_H
