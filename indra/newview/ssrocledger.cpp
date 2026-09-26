/**
 * @file ssrocledger.cpp
 * @brief Region Object Cache Phase 2a: the object ledger and the promotion decision - see doc/region_object_cache.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssrocledger.h"

#include "ssrocghost.h"
#include "ssobjectfacts.h"   // <SS:Nexii/> the shared object cache: set-to-group, and the region navmesh's verdict on permanence
#include "ssparcelfacts.h"   // <SS:Nexii/> the shared parcel cache: owner, group and auto-return for the land under an object
#include "ssrocprobe.h"   // <SS:Nexii/> ROC Phase 1.5: only the full-update counter lives here now; the probe counter moved into probeCache so it can see all three outcomes

#include "llagent.h"
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "lldatapacker.h"
#include "llparcel.h"
#include "llpartdata.h"
#include "llprimitive.h"
#include "llregionflags.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerparcelmgr.h"
#include "llviewerparceloverlay.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "llvocache.h"
#include "llvolume.h"
#include "object_flags.h"

#include <algorithm>
#include <cstring>
#include <ctime>

namespace
{
    // The blob's fixed prefix: FullID at 0 through Owner at 68..83. Every field the ledger records lives inside it and every one is unconditionally present, so recording never has to walk the variable-length tail. Derived from LLViewerObject::initObjectDataMap (llviewerobject.cpp:810-860).
    const S32 SSROC_BLOB_PREFIX_BYTES = 84;

    // The wire buffer an ObjectUpdateCompressed block is copied into is a 2048-byte stack array (llviewerobjectlist.cpp:571-572) while the size handed to assignBuffer comes from getSizeFast, which is not bounded against it. Copying more than this out of a cacheFullUpdate blob would read past that array.
    const S32 SSROC_WIRE_BLOB_MAX = 2048;

    // Faces per object, matching MAX_TES in LLPrimitive::unpackTEMessage (llprimitive.cpp:1499).
    const U8 SSROC_MAX_FACES = 45;

    // What counts as "the object did not move". Generous, because the promotion penalty for drift is a score term and a false positive only slows an object down.
    const F32 SSROC_NEIGHBOURHOOD_M = 1.0f;

    const U32 SSROC_SECS_PER_HOUR = 3600;

    // Connected once, on the ledger's first use. Held here rather than on the class so the header does not have to pull in boost signals.
    boost::signals2::connection s_region_changed_conn;
    boost::signals2::connection s_parcel_changed_conn;

    U16 ssBumpU16(U16 v) { return (v < 65535) ? (U16)(v + 1) : v; }

    // The overlay is one byte per 4m cell and its accessors log a warning for every out-of-range query, so positions are bounds-checked here rather than relying on parcelFlags' own guard (llviewerparceloverlay.cpp:301-312).
    bool ssPosInRegion(const LLViewerRegion* regionp, const LLVector3& pos)
    {
        const F32 w = regionp->getWidth();
        return (w > 0.f
                && pos.mV[VX] >= 0.f && pos.mV[VX] < w
                && pos.mV[VY] >= 0.f && pos.mV[VY] < w);
    }
}

SSROCBlobFacts::SSROCBlobFacts()
:   mLocalID(0),
    mCRC(0),
    mSpecialCode(0),
    mParentLocalID(0),
    mPos(),
    mScale(),
    mPCode(0),
    mState(0)
{
}

// ---------------------------------------------------------------------------
// Blob reading
// ---------------------------------------------------------------------------

bool ssROCParseBlob(const U8* blob, S32 len, SSROCBlobFacts& out)
{
    if (!blob || len < SSROC_BLOB_PREFIX_BYTES) return false;

    // Every static unpacker shifts from the buffer base and resets afterwards (llviewerobject.cpp:863-892), so these are order-independent and leave the packer in a defined state. Only the literal keys from initObjectDataMap are ever passed: sObjectDataMap is a std::map indexed with operator[], so an unknown name would silently insert itself at offset 0 and report the FullID's first bytes as the answer.
    LLDataPackerBinaryBuffer dp(const_cast<U8*>(blob), len);

    LLViewerObject::unpackUUID(&dp, out.mFullID, "ID");
    LLViewerObject::unpackU32(&dp, out.mLocalID, "LocalID");
    LLViewerObject::unpackU8(&dp, out.mPCode, "PCode");
    LLViewerObject::unpackU8(&dp, out.mState, "State");
    LLViewerObject::unpackU32(&dp, out.mCRC, "CRC");
    LLViewerObject::unpackVector3(&dp, out.mScale, "Scale");
    LLViewerObject::unpackVector3(&dp, out.mPos, "Pos");
    LLViewerObject::unpackU32(&dp, out.mSpecialCode, "SpecialCode");
    LLViewerObject::unpackUUID(&dp, out.mOwnerID, "Owner");

    // ParentID is the first field PAST the fixed prefix, and its offset depends on whether Omega is present, so the length needed is checked here rather than letting verifyLength log a warning per object.
    if (out.mSpecialCode & 0x20)
    {
        const S32 needed = (out.mSpecialCode & 0x80) ? 100 : 88;
        if (len >= needed)
        {
            LLViewerObject::unpackParentID(&dp, out.mParentLocalID);
        }
    }

    return out.mFullID.notNull();
}

bool ssROCIsDisqualified(const SSROCBlobFacts& facts, U32 update_flags, S32 blob_len)
{
    // A blob the stock cache itself could not hold can never be rezzed back, so it is not a candidate at any score.
    if (blob_len <= 0 || blob_len > (S32)SSROC_MAX_DP_SIZE) return true;

    if (facts.mPCode == LL_PCODE_LEGACY_AVATAR) return true;

    // State carries the attachment id, but ONLY for volumes: LLVOGrass reads the same byte as its species (llvograss.cpp:94), so testing it unconditionally would disqualify every blade of grass on the grid.
    if (facts.mPCode == LL_PCODE_VOLUME && facts.mState != 0) return true;

    // Physical and pathfinding-character objects move on their own, so no bonus or tenure may resurrect them. Temporary-on-rez never reaches the cache at all (llviewerobjectlist.cpp:609-614) but is tested anyway, because a record written by an older build could carry it.
    if (update_flags & (FLAGS_USE_PHYSICS | FLAGS_CHARACTER | FLAGS_TEMPORARY_ON_REZ)) return true;

    return false;
}

// Mirrors the compressed unpack order in llviewerobject.cpp:1976-2088 exactly and stops at the extra-parameter block.
//
// Diffuse TextureEntry mining is deliberately NOT done here. Reaching the TE block means consuming the volume params first, and LLPrimitive::unpackTEMessage reads getNumTEs() to decide how many faces to parse (llprimitive.cpp:1553) - which is zero on a bare LLPrimitive, so it would harvest nothing without constructing a real volume. It also holds a function-local static (llprimitive.cpp:1502) and so is main-thread-only. The BC7 manifest consumer does not exist until Phase 4, so the risk is not worth taking yet.
void ssROCMineAssets(const std::vector<U8>& blob, std::vector<LLUUID>& out)
{
    if (blob.size() < (size_t)SSROC_BLOB_PREFIX_BYTES) return;
    if (blob.size() > (size_t)SSROC_MAX_DP_SIZE) return;

    // LLDataPackerBinaryBuffer::unpackString calls strlen() BEFORE its own bounds check (lldatapacker.cpp:258), so an unterminated tail would read past the vector. Walking a padded copy means the worst case runs into our own zeroes.
    std::vector<U8> scratch(blob.size() + 8, 0);
    memcpy(scratch.data(), blob.data(), blob.size());

    LLDataPackerBinaryBuffer dp(scratch.data(), (S32)scratch.size());

    U32 special = 0;
    LLViewerObject::unpackU32(&dp, special, "SpecialCode");

    dp.shift(SSROC_BLOB_PREFIX_BYTES);

    LLVector3   v3;
    U32         u32 = 0;
    U8          u8 = 0;
    std::string str;

    if (special & 0x80) { if (!dp.unpackVector3(v3, "Omega")) return; }
    if (special & 0x20) { if (!dp.unpackU32(u32, "ParentID")) return; }

    if (special & 0x2)
    {
        if (!dp.unpackU8(u8, "TreeData")) return;
    }
    else if (special & 0x1)
    {
        if (!dp.unpackU32(u32, "ScratchPadSize")) return;
        std::vector<U8> pad(scratch.size(), 0);
        S32 pad_size = 0;
        if (!dp.unpackBinaryData(pad.data(), (S32)pad.size(), pad_size, "PartData")) return;
    }

    if (special & 0x4)
    {
        if (!dp.unpackString(str, "Text")) return;
        U8 coloru[4];
        if (!dp.unpackBinaryDataFixed(coloru, 4, "Color")) return;
    }

    if (special & 0x200)
    {
        if (!dp.unpackString(str, "MediaURL")) return;
    }

    if (special & 0x8)
    {
        LLPartSysData legacy;
        if (!legacy.unpackLegacy(dp)) return;
        if (legacy.mPartImageID.notNull()) out.push_back(legacy.mPartImageID);
    }

    U8 num_parameters = 0;
    if (!dp.unpackU8(num_parameters, "num_params")) return;

    // Sized to the whole scratch buffer rather than to MAX_OBJECT_PARAMS_SIZE: unpackBinaryData bounds the declared size against the SOURCE only, never the destination (lldatapacker.cpp:292-318), so the destination has to be large enough for any size the source could possibly satisfy.
    std::vector<U8> param_block(scratch.size(), 0);

    for (U8 i = 0; i < num_parameters; ++i)
    {
        U16 param_type = 0;
        S32 param_size = 0;
        if (!dp.unpackU16(param_type, "param_type")) return;
        if (!dp.unpackBinaryData(param_block.data(), (S32)param_block.size(), param_size, "param_data")) return;
        if (param_size <= 0) continue;

        LLDataPackerBinaryBuffer pdp(param_block.data(), param_size);

        switch (param_type)
        {
        case LLNetworkData::PARAMS_SCULPT:
        {
            LLSculptParams p;
            if (p.unpack(pdp) && p.getSculptTexture().notNull()) out.push_back(p.getSculptTexture());
            break;
        }
        case LLNetworkData::PARAMS_LIGHT_IMAGE:
        {
            LLLightImageParams p;
            if (p.unpack(pdp) && p.getLightTexture().notNull()) out.push_back(p.getLightTexture());
            break;
        }
        case LLNetworkData::PARAMS_RENDER_MATERIAL:
        {
            LLRenderMaterialParams p;
            if (p.unpack(pdp))
            {
                for (U8 te = 0; te < SSROC_MAX_FACES; ++te)
                {
                    const LLUUID& id = p.getMaterial(te);
                    if (id.notNull()) out.push_back(id);
                }
            }
            break;
        }
        default:
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Stage A hooks
// ---------------------------------------------------------------------------

void ssROCNoteCacheUpdate(LLViewerRegion* regionp, U32 local_id, U32 crc, U32 flags, LLDataPackerBinaryBuffer& dp)
{
    // <SS:Nexii/> ROC Phase 1.5, above the enabled gate for the same reason as the probe hook. A full update is the simulator declining to use the cache at all, which is the comparison the probe count is only meaningful against.
    ssROCProbeNoteFullUpdate(regionp);

    if (!regionp || !SSROCStore::enabled()) return;

    // The blob must be copied here, not pointed at: dp's buffer is a stack array in the caller and is gone the moment the message loop advances.
    const S32 len = llmin(dp.getBufferSize(), SSROC_WIRE_BLOB_MAX);

    // Reconciliation FIRST, and independent of the ledger: this is the simulator's word about an object, and the ghost standing in for it has to stand down whether or not a ledger exists to record the sighting.
    if (SSROCGhostMgr::instanceExists())
    {
        SSROCGhostMgr::instance().noteSimUpdate(regionp, local_id, crc, flags, dp.getBuffer(), len);
    }

    if (!SSROCLedger::instanceExists()) return;
    SSROCLedger::instance().noteSighting(regionp, local_id, crc, flags, dp.getBuffer(), len);
}

void ssROCNoteCacheProbe(LLViewerRegion* regionp, U32 local_id, U32 crc, U32 flags, LLVOCacheEntry* entry)
{
    if (!regionp || !entry || !SSROCStore::enabled()) return;

    // An ObjectUpdateCached probe carries no blob at all, so the entry's own .slc copy is the only description available. It is copied rather than referenced because LLVOCacheEntry::updateEntry frees and reallocates it, and clearVOCacheFromMemory can drop the whole map while the agent is still standing in the region.
    LLDataPackerBinaryBuffer* edp = entry->getDP();
    if (!edp) return;

    if (SSROCGhostMgr::instanceExists())
    {
        SSROCGhostMgr::instance().noteSimUpdate(regionp, local_id, crc, flags, edp->getBuffer(), edp->getBufferSize());
    }

    if (!SSROCLedger::instanceExists()) return;
    SSROCLedger::instance().noteSighting(regionp, local_id, crc, flags, edp->getBuffer(), edp->getBufferSize());
}

// ---------------------------------------------------------------------------
// SSROCLedger
// ---------------------------------------------------------------------------

SSROCLedger::RegionState::RegionState()
:   mHandle(0),
    mSandbox(false),
    mSandboxParcelName(false),
    mLoadResolved(false),
    mHaveHandshake(false),
    mEpochApplied(false),
    mStaleMarked(0),
    mVisitSecs(0),
    mTrackedStart(0.0),
    mTrackedSecs(0),
    mConfirmed(0),
    mCreated(0),
    mNoBlob(0),
    mOwnerLookups(0),
    mPromotedThisVisit(0)
{
}

SSROCLedger::SSROCLedger()
:   mDwellHandle(0),
    mDwellStart(0.0),
    mDwellHooked(false)
{
}

SSROCLedger::~SSROCLedger()
{
}

void SSROCLedger::shutdown()
{
    if (mMetrics.mSightings || mMetrics.mRecordsCreated)
    {
        LL_INFOS("SSROC") << metricsString() << LL_ENDL;
    }

    s_region_changed_conn.disconnect();
    s_parcel_changed_conn.disconnect();
    mDwellHooked = false;
    mDwellHandle = 0;

    mRegions.clear();
    mOwnerClass.clear();
}

SSROCLedger::RegionState* SSROCLedger::stateFor(U64 handle)
{
    auto it = mRegions.find(handle);
    return (it == mRegions.end()) ? NULL : &it->second;
}

void SSROCLedger::onRegionAdded(LLViewerRegion* regionp)
{
    if (!regionp || !SSROCStore::enabled()) return;

    if (!mDwellHooked)
    {
        // Dwell means seconds the AGENT was present, not seconds the circuit was up. Neighbour regions are loaded for a whole session and never occupied, so accumulating from region-add would credit them with hours the user never spent there. LLViewerRegion::mRegionTimer is no use either: it is private, and it resets on every re-received RegionHandshake (llviewerregion.cpp:3464).
        s_region_changed_conn = gAgent.addRegionChangedCallback(
            []() { if (SSROCLedger::instanceExists()) SSROCLedger::instance().onAgentRegionChanged(); });
        s_parcel_changed_conn = gAgent.addParcelChangedCallback(
            []() { if (SSROCLedger::instanceExists()) SSROCLedger::instance().onAgentParcelChanged(); });
        mDwellHooked = true;

        // The agent is normally already standing somewhere by the time the first region is added, and setRegion for that region has already fired.
        onAgentRegionChanged();
    }

    const U64 handle = regionp->getHandle();
    if (mRegions.find(handle) != mRegions.end()) return;

    RegionState& rs = mRegions[handle];
    rs.mHandle   = handle;
    rs.mRegionID = regionp->getRegionID();
    rs.mSandbox  = regionp->getRegionFlag(REGION_FLAGS_SANDBOX);

    // <SS:Nexii> The region's own lifetime clock, started where the region becomes findable by handle. This is what the settled-stay gate is measured against, and it is deliberately NOT the dwell clock above: a region streams its objects to the viewer for as long as it is in the world, whether or not the agent ever stands in it, and a gate asking "did this visit observe enough to decide anything" has to be answered by observation rather than by the agent's feet. LLViewerRegion::mRegionTimer would be the exact stock equivalent but is private and resets on every re-received handshake, so the clock is kept here instead.
    rs.mTrackedStart = (F64)LLTimer::getElapsedSeconds();

    ++mMetrics.mRegionsTracked;
}

void SSROCLedger::onRegionFileLoaded(U64 handle, SSROCFilePtr file)
{
    if (!SSROCStore::enabled()) return;

    RegionState* rs = stateFor(handle);
    if (!rs) return;

    rs->mLoadResolved = true;
    if (!file)
    {
        // Still a resolution, so the epoch decision must not be left waiting on a file that is never coming.
        applyIDEpoch(*rs);
        return;
    }

    // The region id is checked rather than assumed: handles are reused across grids and sessions, and adopting another region's ledger would be both wrong and very hard to diagnose.
    //
    // It is re-resolved from the live region here rather than trusting the value sampled at LLWorld::addRegion: setRegionID only runs inside unpackRegionHandshake, which has not happened at that point, so the sampled copy is always null and the guard below would short-circuit on its own second clause every single time.
    LLViewerRegion* live = LLWorld::instanceExists() ? LLWorld::getInstance()->getRegionFromHandle(handle) : NULL;
    if (live && live->getRegionID().notNull())
    {
        rs->mRegionID = live->getRegionID();
    }

    if (file->mRegionID.notNull() && rs->mRegionID.notNull() && file->mRegionID != rs->mRegionID)
    {
        applyIDEpoch(*rs);
        return;
    }

    // <SS:Nexii/> The shared object cache is primed with what this region answered on previous visits, before a single sighting is recorded, so the select probe never asks again about an object already answered for. FullID-keyed and epoch-independent, so unlike the local ids below these need no staleness pass.
    for (const auto& pair : file->mGroups) ssObjectFactsSeedGroup(handle, pair.first, pair.second);

    // The id epoch the records on disk were written under, latched only once the file has been accepted as this region's. Kept beside the live one rather than compared here, because the handshake may not have arrived yet.
    rs->mFileCacheID = file->mLastCacheID;

    // Records created before the disk read landed keep whatever this visit already learned; everything else is adopted from disk. Merging by FullID rather than replacing is what makes the race harmless in both directions.
    for (const SSROCRecord& disk_rec : file->mRecords)
    {
        if (disk_rec.mFullID.isNull()) continue;

        auto found = rs->mByFullID.find(disk_rec.mFullID);
        if (found == rs->mByFullID.end())
        {
            const U32 index = (U32)rs->mRecords.size();
            rs->mRecords.push_back(disk_rec);
            RegionState::Live live;
            live.mParentLocalID = 0;
            live.mNoted = 0;
            rs->mLive.push_back(live);
            rs->mByFullID[disk_rec.mFullID] = index;
        }
        else
        {
            // Already sighted this visit. Take the cross-session ledger from disk, keep this visit's fresh description.
            SSROCRecord& live_rec = rs->mRecords[found->second];
            const U32   live_flags  = live_rec.mRecordFlags;
            const bool  live_noted  = rs->mLive[found->second].mNoted != 0;

            std::vector<U8> keep_blob;
            keep_blob.swap(live_rec.mDP);
            const U32       keep_crc   = live_rec.mCRC;
            const U32       keep_local = live_rec.mLastLocalID;
            const U32       keep_upd   = live_rec.mUpdateFlags;
            const LLVector3 keep_pos   = live_rec.mPos;
            const LLVector3 keep_scale = live_rec.mScale;
            const LLUUID    keep_owner = live_rec.mOwnerID;

            live_rec = disk_rec;
            if (!keep_blob.empty()) live_rec.mDP.swap(keep_blob);
            live_rec.mCRC         = keep_crc;
            live_rec.mLastLocalID = keep_local;
            live_rec.mUpdateFlags = keep_upd;
            live_rec.mPos         = keep_pos;
            live_rec.mScale       = keep_scale;
            if (keep_owner.notNull()) live_rec.mOwnerID = keep_owner;
            live_rec.mRecordFlags = disk_rec.mRecordFlags | (live_flags & (SSROC_REC_DIRTY | SSROC_REC_DISQUALIFIED | SSROC_REC_IS_CHILD | SSROC_REC_OWNER_PUBLIC_WORKS));

            if (live_noted)
            {
                // The confirmation this visit already happened, so replay it onto the ledger that just arrived from disk rather than losing it.
                live_rec.noteEntry();
                live_rec.markSeenOn((U64)time(NULL));
                live_rec.mSessionsSeen = ssBumpU16(live_rec.mSessionsSeen);

                // And the id this visit's sighting brought with it is the simulator's own, so a stale mark carried in from disk is answered by the very sighting that landed before the read did.
                live_rec.mRecordFlags |=  SSROC_REC_ID_CURRENT;
                live_rec.mRecordFlags &= ~SSROC_REC_ID_STALE;
            }
        }
    }

    LL_DEBUGS("SSROC") << "Ledger seeded for region " << handle << " with " << file->mRecords.size()
                       << " records from disk, " << rs->mRecords.size() << " live" << LL_ENDL;

    // The records the epoch decision is about have only just arrived, so this is the second of the two arrival orders.
    applyIDEpoch(*rs);
}

void SSROCLedger::onRegionHandshake(LLViewerRegion* regionp, const LLUUID& cache_id)
{
    if (!regionp || !SSROCStore::enabled()) return;

    RegionState* rs = stateFor(regionp->getHandle());
    if (!rs) return;

    rs->mCacheID       = cache_id;
    rs->mHaveHandshake = true;

    applyIDEpoch(*rs);
}

void SSROCLedger::applyIDEpoch(RegionState& rs)
{
    // Both facts are needed and they race in both directions, so this is driven from whichever arrives second. Until then nothing is decided, which is the safe direction: an undecided record is simply not yet marked, and the marking is what the NEXT visit reads.
    if (rs.mEpochApplied || !rs.mHaveHandshake || !rs.mLoadResolved) return;
    rs.mEpochApplied = true;

    // The one case where every stored id is still good. Anything else - a genuine restart, or a previous visit that never saw a handshake and so could not vouch for its own ids - means the file's ids are not evidence about the current epoch.
    const bool same_epoch = rs.mFileCacheID.notNull() && rs.mCacheID.notNull() && rs.mFileCacheID == rs.mCacheID;
    if (same_epoch) return;

    // "Confirmed this visit" is the exact test, not the CURRENT bit: that bit can have been written by a previous visit under a CacheID that has since died, and it is precisely those records - carried forward unmentioned across a restart - that the whole flag exists to catch.
    //
    // The two vectors are grown together at every insertion point, so this bound is belt and braces - but a desync here would index past the end of mLive while deciding whether a remembered object may be painted at a stored id, which is not a place to trust an invariant that costs one comparison to check.
    const size_t n = llmin(rs.mRecords.size(), rs.mLive.size());
    for (size_t i = 0; i < n; ++i)
    {
        if (rs.mLive[i].mNoted)
        {
            rs.mRecords[i].mRecordFlags |=  SSROC_REC_ID_CURRENT;
            rs.mRecords[i].mRecordFlags &= ~SSROC_REC_ID_STALE;
            continue;
        }

        if (rs.mRecords[i].mRecordFlags & SSROC_REC_ID_STALE) continue;

        rs.mRecords[i].mRecordFlags &= ~SSROC_REC_ID_CURRENT;
        rs.mRecords[i].mRecordFlags |=  SSROC_REC_ID_STALE;
        ++rs.mStaleMarked;
    }

    if (rs.mStaleMarked)
    {
        LL_INFOS("SSROC") << "Region " << rs.mHandle << ": the simulator's cache id is " << rs.mCacheID
                          << " but this region's records were stored under " << rs.mFileCacheID
                          << ", so " << rs.mStaleMarked << " of " << n
                          << " stored local ids now name different objects and will not be painted until the simulator mentions them again" << LL_ENDL;
    }
}

void SSROCLedger::onAgentRegionChanged()
{
    LLViewerRegion* regionp = gAgent.getRegion();
    const U64 handle = regionp ? regionp->getHandle() : 0;

    // LLAgent::setRegion fires this signal unconditionally, outside its own changed test (llagent.cpp:1320), so it has to be idempotent or a stationary agent would restart the clock every time anything called setRegion.
    if (handle == mDwellHandle) return;

    flushDwell();

    mDwellHandle = handle;
    mDwellStart  = (F64)LLTimer::getElapsedSeconds();

    // The parcel-changed signal does not fire on a teleport into a new region, so sample here too.
    onAgentParcelChanged();
}

void SSROCLedger::onAgentParcelChanged()
{
    if (!SSROCStore::enabled()) return;

    LLViewerRegion* regionp = gAgent.getRegion();
    if (!regionp) return;

    RegionState* rs = stateFor(regionp->getHandle());
    if (!rs || rs->mSandboxParcelName) return;

    if (!LLViewerParcelMgr::instanceExists()) return;
    LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
    if (!parcel) return;

    // A parcel named "sandbox" is a heuristic, not a fact, so it never skips recording - it only denies the structural immunity credit, which drops those objects onto the conservative distinct-days path. In Phase 2a objects are not attributed to parcels yet, so this is applied region-wide: it is only ever raised by a parcel the agent actually stood on, and denying too much costs patience rather than correctness.
    if (ssROCParcelNameDeniesImmunity(parcel->getName()))
    {
        rs->mSandboxParcelName = true;
        LL_INFOS("SSROC") << "Region " << rs->mHandle << ": parcel \"" << parcel->getName()
                          << "\" denies the auto-return immunity credit for this visit" << LL_ENDL;
    }
}

void SSROCLedger::flushDwell()
{
    if (!mDwellHandle) return;

    RegionState* rs = stateFor(mDwellHandle);
    if (rs)
    {
        const F64 now = (F64)LLTimer::getElapsedSeconds();
        const F64 span = now - mDwellStart;
        if (span > 0.0 && span < 86400.0 * 30.0)
        {
            rs->mVisitSecs += (U32)span;
        }
    }
    mDwellHandle = 0;
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

U32 SSROCLedger::addRecord(RegionState& rs, const SSROCBlobFacts& facts, U32 flags, const U8* blob, S32 blob_len)
{
    const U32 index = (U32)rs.mRecords.size();

    rs.mRecords.push_back(SSROCRecord());
    SSROCRecord& rec = rs.mRecords.back();

    rec.mFullID      = facts.mFullID;
    rec.mOwnerID     = facts.mOwnerID;
    rec.mLastLocalID = facts.mLocalID;
    rec.mCRC         = facts.mCRC;
    rec.mUpdateFlags = flags;
    rec.mPos         = facts.mPos;
    rec.mScale       = facts.mScale;

    if (blob && blob_len > 0 && blob_len <= (S32)SSROC_MAX_DP_SIZE)
    {
        rec.mDP.assign(blob, blob + blob_len);
    }

    if (facts.mParentLocalID != 0)
    {
        rec.mRecordFlags |= SSROC_REC_IS_CHILD;
    }
    if (ssROCIsDisqualified(facts, flags, (S32)rec.mDP.size()))
    {
        rec.mRecordFlags |= SSROC_REC_DISQUALIFIED;
    }
    rec.mRecordFlags |= SSROC_REC_DIRTY;

    // The id came straight off the wire under the CacheID in force right now, which is the strongest possible statement that it is current.
    rec.mRecordFlags |= SSROC_REC_ID_CURRENT;

    RegionState::Live live;
    live.mParentLocalID = facts.mParentLocalID;
    live.mNoted = 0;
    rs.mLive.push_back(live);

    rs.mByFullID[facts.mFullID] = index;

    ++rs.mCreated;
    ++mMetrics.mRecordsCreated;

    return index;
}

void SSROCLedger::confirmRecord(RegionState& rs, U32 index, U32 local_id, U32 crc, U32 flags, U64 now)
{
    SSROCRecord& rec = rs.mRecords[index];

    rec.mLastLocalID = local_id;
    rec.mUpdateFlags = flags;
    rec.mCRC         = crc;
    rec.mConfirmCount = ssBumpU16(rec.mConfirmCount);

    // Re-keyed by the simulator's own stream, so whatever epoch the stored id used to belong to no longer matters - this is the one thing that can rescue a record written off after a restart, and it is why a stale mark is a deferral rather than a death sentence.
    rec.mRecordFlags |=  SSROC_REC_ID_CURRENT;
    rec.mRecordFlags &= ~SSROC_REC_ID_STALE;

    // Flags are live and can change between visits, so the hard gate is re-evaluated rather than latched. Nothing ever clears it: an object that was physical once is not trusted to have stopped.
    if (flags & (FLAGS_USE_PHYSICS | FLAGS_CHARACTER | FLAGS_TEMPORARY_ON_REZ))
    {
        rec.mRecordFlags |= SSROC_REC_DISQUALIFIED;
    }

    if (!rs.mLive[index].mNoted)
    {
        // probeCache can be called more than once for the same object at the same CRC within a visit - the "already probed" early-out at llviewerregion.cpp:3116-3119 exists precisely because it happens - and the hook sits above that guard. Counting entries and days exactly once per visit is what keeps the visit currency meaningful.
        rs.mLive[index].mNoted = 1;
        rec.noteEntry();
        rec.markSeenOn(now);
        rec.mSessionsSeen = ssBumpU16(rec.mSessionsSeen);
        ++rs.mConfirmed;
    }
    else
    {
        // markSeenOn rather than writing mLastConfirmed directly: the day bitmap's anchor IS mLastConfirmed, so moving it without shifting the bitmap makes bit zero mean a different day than the bit that is actually set. For anyone whose session crosses UTC midnight that freezes the distinct-day count at one forever, and because tenureDays reads the same field the score still climbs - so it looks like thresholds are too strict rather than like a stopped clock. markSeenOn is idempotent within a day and keeps the backwards-clock guard.
        rec.markSeenOn(now);
    }
}

U32 SSROCLedger::seedObjectCache(U64 handle, U32 max_seed, const std::function<bool(const SSROCRecord&)>& sink, SSROCSeedStats& stats, U8& outcome)
{
    outcome = SSROC_BACK_DISABLED;
    if (!SSROCStore::enabled() || !sink) return 0;

    RegionState* rsp = stateFor(handle);
    if (!rsp)                { outcome = SSROC_BACK_NOT_TRACKED;     return 0; }

    RegionState& rs = *rsp;

    // A sandbox region records nothing, so there is nothing to fill from and the stock path has to serve it. Withholding a cache from a sandbox would not make the viewer safer, it would make it ask the simulator for every object on every visit.
    if (rs.mSandbox)         { outcome = SSROC_BACK_SANDBOX;         return 0; }
    if (!rs.mHaveHandshake)  { outcome = SSROC_BACK_NO_HANDSHAKE;    return 0; }
    if (!rs.mLoadResolved)   { outcome = SSROC_BACK_LOAD_UNRESOLVED; return 0; }
    if (!rs.mEpochApplied)   { outcome = SSROC_BACK_EPOCH_UNDECIDED; return 0; }
    if (rs.mRecords.empty()) { outcome = SSROC_BACK_NO_RECORDS;      return 0; }

    // Both ids are reported by the caller rather than folded into one boolean, because "the simulator restarted" and "the visit that wrote this file never saw a handshake" are materially different situations that used to print identically on the ghost path and cost this project several sessions of misdiagnosis.
    const bool same_epoch = rs.mFileCacheID.notNull() && rs.mCacheID.notNull() && rs.mFileCacheID == rs.mCacheID;
    if (!same_epoch)
    {
        outcome = rs.mFileCacheID.isNull() ? SSROC_BACK_EPOCH_UNKNOWN : SSROC_BACK_CACHEID_CHANGED;
        return 0;
    }

    std::vector<U32> plan;
    ssROCBuildSeedPlan(rs.mRecords, same_epoch, max_seed, plan, stats);

    if (plan.empty())        { outcome = SSROC_BACK_PLAN_EMPTY;      return 0; }

    U32 seeded = 0;
    for (U32 index : plan)
    {
        if (index >= rs.mRecords.size()) continue;
        if (sink(rs.mRecords[index])) ++seeded;
    }

    outcome = SSROC_BACK_SEEDED;
    return seeded;
}

void SSROCLedger::noteSighting(LLViewerRegion* regionp, U32 local_id, U32 crc, U32 flags, const U8* blob, S32 blob_len)
{
    RegionState* rsp = stateFor(regionp->getHandle());
    if (!rsp) return;

    RegionState& rs = *rsp;

    // Re-checked rather than latched: only Linden Lab can set this flag, but it can change mid-session through the RegionInfo message without a fresh handshake (llviewermessage.cpp:5380). A sandbox region records nothing at all - its object content is transient by definition and is the worst possible thing to rez back as scenery.
    if (rs.mSandbox || regionp->getRegionFlag(REGION_FLAGS_SANDBOX))
    {
        if (!rs.mSandbox)
        {
            rs.mSandbox = true;
            ++mMetrics.mRegionsSandbox;
            LL_INFOS("SSROC") << "Region " << rs.mHandle << " is a sandbox - recording nothing" << LL_ENDL;
        }
        return;
    }

    ++mMetrics.mSightings;

    const U64 now = (U64)time(NULL);

    // Fast path: this local id is already bound to a record and nothing about it changed. No parse, no allocation, no map insert.
    auto bound = rs.mByLocalID.find(local_id);
    if (bound != rs.mByLocalID.end())
    {
        SSROCRecord& rec = rs.mRecords[bound->second];
        if (rec.mCRC == crc)
        {
            confirmRecord(rs, bound->second, local_id, crc, flags, now);
            return;
        }
    }

    SSROCBlobFacts facts;
    if (!ssROCParseBlob(blob, blob_len, facts))
    {
        // No usable description, so there is nothing to key a FullID-addressed record on. Counted rather than guessed at.
        ++rs.mNoBlob;
        ++mMetrics.mNoBlob;
        return;
    }

    // <SS:Nexii/> Stage B: the parcel under this object is a candidate for the throttled immunity probe. Roots only - a child's Pos is parent-relative (llvocache.cpp:701-706) and would file most of a linkset at the region origin. O(1), and it returns on one indexed byte for every sighting after the first in a given parcel, which is nearly all of them.
    if (facts.mParentLocalID == 0)
    {
        ssParcelFactsRequest(regionp, facts.mPos);

        // The withdrawal filter is built only where a request would actually be queued. It captures three values and therefore costs a heap allocation, and this path runs thousands of times a second - paying for a functor that the very next line would discard is exactly the kind of cost that does not show up in a profile as itself.
        if (ssObjectFactsWouldQueue(rs.mHandle, facts.mFullID))
        {
            const U64       want_handle = rs.mHandle;
            const LLVector3 want_pos    = facts.mPos;
            const LLUUID    want_owner  = facts.mOwnerID;

            ssObjectFactsRequest(regionp, facts.mLocalID, facts.mFullID, false,
                [want_handle, want_pos, want_owner]() -> bool
                {
                    LLViewerRegion* live = LLWorld::instanceExists() ? LLWorld::getInstance()->getRegionFromHandle(want_handle) : NULL;
                    if (!live) return false;

                    // No parcel answer yet, so ask anyway: the answer keeps, and asking now costs one message where asking after the parcel lands costs the same message plus a visit's delay.
                    SSParcelFacts parcel;
                    if (!ssParcelFactsGet(live, want_pos, parcel)) return true;

                    // Two refusals, both free, and together they take most of a typical region off the wire. The parcel is set to no group, so the set-to-group bucket is empty by construction and no answer could match it; or the object's owner IS the parcel owner, so it is already immune by a leg that cost nothing.
                    if (parcel.mGroupID.isNull()) return false;
                    if (want_owner.notNull() && parcel.mOwnerID.notNull() && want_owner == parcel.mOwnerID) return false;
                    return true;
                });
        }
    }

    auto known = rs.mByFullID.find(facts.mFullID);
    U32 index;

    if (known == rs.mByFullID.end())
    {
        index = addRecord(rs, facts, flags, blob, blob_len);
        classifyOwner(rs, facts.mOwnerID);
    }
    else
    {
        index = known->second;
        SSROCRecord& rec = rs.mRecords[index];

        // The stored local id is refreshed on every sighting while the blob is only replaced here, so an object whose id moved but whose CRC did not - the ordinary case for the first sighting of a record after a simulator restart - would otherwise keep a blob naming the id it had in the previous epoch. That disagreement is harmless while the blob is only ever rezzed, and is not harmless once the same record answers the simulator's cache probes, so the two are kept in lockstep at the one place that can do it.
        if (rec.mCRC != crc || rec.mDP.empty() || rec.mLastLocalID != facts.mLocalID)
        {
            if (rec.mCRC != crc && rec.mCRC != 0)
            {
                rec.mCRCChangeCount = ssBumpU16(rec.mCRCChangeCount);
            }

            if (blob && blob_len > 0 && blob_len <= (S32)SSROC_MAX_DP_SIZE)
            {
                rec.mDP.assign(blob, blob + blob_len);
            }
            else
            {
                // The stored blob is dropped rather than kept, because confirmRecord below is about to write the NEW crc onto this record and a record whose crc and blob disagree is worse than one with no blob at all: it would answer a probe with a match and then rez the previous description of the object. A blobless record still keeps its whole ledger and re-acquires a blob the next time the object is described in full.
                rec.mDP.clear();
            }

            // Drift is measured against the record's own last known position, not against a per-session origin, so it survives sim restarts exactly as the FullID key does.
            const F32 radius = facts.mScale.length() * 0.5f;
            const F32 allow  = llmax(SSROC_NEIGHBOURHOOD_M, radius * 0.25f);
            if ((facts.mPos - rec.mPos).length() > allow)
            {
                rec.mMoveCount = ssBumpU16(rec.mMoveCount);
            }

            rec.mPos   = facts.mPos;
            rec.mScale = facts.mScale;
            rec.mRecordFlags |= SSROC_REC_DIRTY;

            if (facts.mParentLocalID != 0) rec.mRecordFlags |= SSROC_REC_IS_CHILD;
            if (ssROCIsDisqualified(facts, flags, (S32)rec.mDP.size())) rec.mRecordFlags |= SSROC_REC_DISQUALIFIED;
        }

        if (rec.mOwnerID.isNull() && facts.mOwnerID.notNull())
        {
            rec.mOwnerID = facts.mOwnerID;
            classifyOwner(rs, facts.mOwnerID);
        }

        rs.mLive[index].mParentLocalID = facts.mParentLocalID;
    }

    rs.mByLocalID[local_id] = index;
    confirmRecord(rs, index, local_id, crc, flags, now);
}

// ---------------------------------------------------------------------------
// Owner classification
// ---------------------------------------------------------------------------

U8 SSROCLedger::classifyOwner(RegionState& rs, const LLUUID& owner_id)
{
    if (owner_id.isNull()) return OWNER_UNKNOWN;

    auto memo = mOwnerClass.find(owner_id);
    if (memo != mOwnerClass.end() && memo->second != OWNER_UNKNOWN) return memo->second;

    // LLAvatarNameCache::get is a query AND a fetch trigger: a miss inserts into its ask queue (llavatarnamecache.cpp:705-709) and its idle drains ~90 ids per HTTP batch. Distinct owners are deduped by the process-lifetime memo above, and the per-region cap bounds a cold region to a couple of batches. Priming during the visit is deliberate - by the time the exit pass reads the memo, the answers have usually arrived.
    static LLCachedControl<U32> max_lookups(gSavedSettings, "SSROCMaxOwnerLookups", 128);
    if (rs.mOwnerLookups >= (U32)max_lookups && memo == mOwnerClass.end()) return OWNER_UNKNOWN;

    LLAvatarName av_name;
    if (!LLAvatarNameCache::get(owner_id, &av_name))
    {
        if (memo == mOwnerClass.end())
        {
            mOwnerClass[owner_id] = OWNER_UNKNOWN;
            ++rs.mOwnerLookups;
            ++mMetrics.mOwnerLookups;
        }
        return OWNER_UNKNOWN;
    }

    // Error paths insert a temporary placeholder name, which must never be tested against - it would classify an unresolved owner as an ordinary resident permanently.
    if (!av_name.isValidName()) return OWNER_UNKNOWN;

    const U8 result = ssROCOwnerIsPublicWorks(av_name.getLegacyName()) ? OWNER_PUBLIC_WORKS : OWNER_ORDINARY;
    mOwnerClass[owner_id] = result;
    return result;
}

// ---------------------------------------------------------------------------
// Stage D - promotion
// ---------------------------------------------------------------------------

F32 SSROCLedger::scoreRecord(const SSROCRecord& rec) const
{
    // Persistence. The bridge's rez age does not exist in Phase 2a, so the cross-visit span carries this term alone, at the 0.75 discount the design gives an inferred signal.
    const F32 persistence = llmin((F32)rec.tenureDays(), 30.f) / 30.f * 0.75f;

    // Familiarity. Sessions rather than confirmations, so a region visited on five separate occasions outranks one hammered by a single probe flood.
    const F32 familiarity = llmin((F32)rec.mSessionsSeen, 5.f) / 5.f;

    // Stability. CRC churn and position drift each cost half the term.
    const F32 churn = llmin((F32)rec.mCRCChangeCount, 8.f) / 8.f;
    const F32 drift = llmin((F32)rec.mMoveCount, 4.f) / 4.f;
    const F32 stability = llclamp(1.f - 0.5f * churn - 0.5f * drift, 0.f, 1.f);

    // Confirmation. The design names the .slc hit and dupe counters, but those live on LLVOCacheEntry, die with the .slc and are unreachable from here - mCacheMap is defined inside llviewerregion.cpp. This counts the same events on the ROC record instead, which additionally survives a cache wipe.
    const F32 confirmation = llmin((F32)rec.mConfirmCount, 20.f) / 20.f;

    F32 score = 0.40f * persistence + 0.25f * familiarity + 0.20f * stability + 0.15f * confirmation;

    if (rec.mRecordFlags & SSROC_REC_OWNER_PUBLIC_WORKS)
    {
        static LLCachedControl<F32> pw_bonus(gSavedSettings, "SSROCPublicWorksBonus", 0.25f);
        score += (F32)pw_bonus;
    }

    // The object shapes the region's navigation mesh. That is not an inference drawn from watching something sit still - it is a declaration by whoever built it, enforced by the simulator, and it names exactly the population this cache exists for: terrain furniture, buildings, roads, the walls a pathfinding character has to walk around.
    //
    // FLAGS_AFFECTS_NAVMESH is an ordinary object update flag (object_flags.h:44) and has been in mUpdateFlags since Stage A. It is what the build floater shows as "Pathfinding attributes: Permanent" (llpanelpermissions.cpp:436, via LLViewerObject::flagObjectPermanent). It therefore costs NOTHING - no probe, no capability, no select, every object, every region, already persisted with the record.
    //
    // An earlier version of this took the same signal from the ObjectLinksets capability instead, which was wrong twice over: that capability answers only for objects the agent owns or can modify - so on other people's builds, which are the entire population this cache exists for, it reported nothing - and it is only addressable for the agent's own region. What that capability does add is the CATEGORY (walkable floor versus static obstacle versus material or exclusion volume), which no update flag carries; that remains available through the shared object cache, for your own objects, and nothing here depends on it.
    if (rec.mUpdateFlags & FLAGS_AFFECTS_NAVMESH)
    {
        static LLCachedControl<F32> navmesh_bonus(gSavedSettings, "SSROCNavmeshPermanentBonus", 0.25f);
        score += (F32)navmesh_bonus;
    }

    if (rec.mUpdateFlags & FLAGS_SCRIPTED)      score -= 0.10f;
    if (rec.mUpdateFlags & FLAGS_HANDLE_TOUCH)  score -= 0.05f;

    // Particles (legacy 0x8 or modern 0x400), omega and attached sound all read out of the SpecialCode word already stored in the blob, so this costs one unpack rather than a walk.
    if (rec.mDP.size() >= (size_t)SSROC_BLOB_PREFIX_BYTES)
    {
        U32 special = 0;
        LLDataPackerBinaryBuffer dp(const_cast<U8*>(rec.mDP.data()), (S32)rec.mDP.size());
        LLViewerObject::unpackU32(&dp, special, "SpecialCode");
        if (special & (0x8 | 0x400 | 0x80 | 0x10)) score -= 0.05f;
    }

    return llclamp(score, 0.f, 1.f);
}

bool SSROCLedger::hasImmunity(LLViewerRegion* regionp, const RegionState& rs, const SSROCRecord& rec) const
{
    // A parcel the agent stood on this visit was named "sandbox". The name is only a heuristic, so it never blocks recording - it denies the credit and lets real days do the work instead.
    if (rs.mSandboxParcelName) return false;

    // Moles build the permanent landscape of the mainland by arrangement with the estate owner, so the owner-match test - which compares against the PARCEL owner, usually Governor Linden rather than the Mole - would miss them entirely. They get the credit outright.
    if (rec.mRecordFlags & SSROC_REC_OWNER_PUBLIC_WORKS) return true;

    // Only a root has a region-local position: a child's Pos is relative to its parent (llvocache.cpp:701-706), so testing a child against a parcel would file most of a linkset at the region origin. Children ride their root's verdict instead.
    if (!rec.isRoot()) return false;
    if (!ssPosInRegion(regionp, rec.mPos)) return false;

    // Tier 0, and free. Your own object on your own land: auto-return only ever takes objects owned by OTHERS, so both halves are required and neither alone is enough. FLAGS_OBJECT_YOU_OWNER is checked alongside the blob owner because the simulator's population of the blob's Owner field for third-party objects is the design's one unverified Tier 0 dependency, and this leg must not depend on it. PARCEL_SELF is the simulator's own answer to "is this cell on a parcel you own", one byte per 4m, unsolicited, no request of any kind - and a region whose overlay never arrived reads as entirely public, which denies the credit, the safe direction.
    const bool mine = (rec.mUpdateFlags & FLAGS_OBJECT_YOU_OWNER)
                   || (rec.mOwnerID.notNull() && rec.mOwnerID == gAgent.getID());

    const LLViewerParcelOverlay* overlay = regionp->getParcelOverlay();
    if (mine && overlay && overlay->isOwnedSelf(rec.mPos)) return true;

    // Tier 1, one metered request per DISTINCT PARCEL per visit (ssparcelfacts.cpp, shared). This is the leg that covers other people's builds, which is the entire population this cache exists for - the overlay above can only ever answer for the agent's own land. A landowner's own content cannot be auto-returned from their own parcel, and that becomes decidable the moment the parcel's owner is known.
    SSParcelFacts parcel_facts;
    if (ssParcelFactsGet(regionp, rec.mPos, parcel_facts))
    {
        // Both sides must be real. A simulator that withholds the parcel owner from an agent with no rights there sends a null, and a blob whose Owner field was never populated stores a null - two nulls comparing equal would manufacture immunity for every object on every unanswered parcel, which is the one failure direction this gate must not have.
        if (rec.mOwnerID.notNull() && parcel_facts.mOwnerID.notNull() && rec.mOwnerID == parcel_facts.mOwnerID) return true;

        // Deeded objects come free with the same reply. FLAGS_OBJECT_GROUP_OWNED (object_flags.h:51) means the blob's Owner field IS a group UUID, so a deeded object standing on a parcel set to that same group is spared by auto-return - the About Land Objects tab's three buckets are "owned by the parcel owner", "set to group" and "owned by others", and only the last is swept. This leg needs no object probe at all; only the set-to-group case, where the object keeps a resident owner and carries a separate GroupID that appears in no object update, still waits on Stage C.
        if ((rec.mUpdateFlags & FLAGS_OBJECT_GROUP_OWNED) && rec.mOwnerID.notNull()
            && parcel_facts.mGroupID.notNull() && rec.mOwnerID == parcel_facts.mGroupID) return true;

        // Tier 2: the set-to-group bucket. An object's GroupID is in no cache blob and no object update - only an ObjectProperties reply carries it, which is what the shared object cache goes and gets. Both sides must be real for the same reason as above: a parcel with no group and an object with no group must not compare equal into a credit.
        SSObjectFacts object_facts;
        if (ssObjectFactsGet(rs.mHandle, rec.mFullID, object_facts)
            && object_facts.mGroupID.notNull() && parcel_facts.mGroupID.notNull()
            && object_facts.mGroupID == parcel_facts.mGroupID)
        {
            return true;
        }

        // Deliberately absent: mCleanOtherTime == 0. Zero means auto-return is disarmed and it equally means the field was withheld, and nothing in the reply distinguishes the two - so trusting it would immunise a whole parcel of other people's furniture standing on land that returns it every fifteen minutes. Disarmed auto-return is corroborating evidence at best, and the distinct-days path already collects the corroboration. Set-to-group immunity is likewise absent for a different reason: an object's group is in no cache blob and no object update, and only Stage C's ObjectProperties probe can supply it.
    }

    return false;
}

void SSROCLedger::runPromotion(LLViewerRegion* regionp, RegionState& rs)
{
    static LLCachedControl<U32> settled_secs(gSavedSettings, "SSROCSettledStaySecs", 180);
    static LLCachedControl<F32> visit_hours(gSavedSettings, "SSROCVisitHours", 2.0f);
    static LLCachedControl<U32> promote_visits(gSavedSettings, "SSROCPromoteVisits", 2);
    static LLCachedControl<U32> promote_days(gSavedSettings, "SSROCPromoteDaysUnproven", 3);
    static LLCachedControl<F32> promote_score(gSavedSettings, "SSROCPromoteScore", 0.40f);
    static LLCachedControl<bool> require_proof(gSavedSettings, "SSROCRequireAutoReturnProof", false);
    // The SAME key the injector declares (ssrocghost.cpp), and it must carry the same fallback: two different defaults for one setting mean the promotion cap and the injection cap silently disagree on any profile where the key is missing, which is every profile written before the setting existed.
    static LLCachedControl<U32> max_ghosts(gSavedSettings, "SSROCMaxGhostsPerRegion", 30000);

    // Every threshold is pulled into a plain local first, so the comparisons below are ordinary integer and float tests rather than conversions through LLCachedControl.
    const U32    settled_need   = (U32)settled_secs;
    const U32    visits_need    = (U32)promote_visits;
    const U32    days_need      = (U32)promote_days;
    const F32    score_need     = (F32)promote_score;
    const bool   proof_required = (bool)require_proof;
    const size_t ghost_cap      = (size_t)(U32)max_ghosts;

    // <SS:Nexii> Two settled tests, because they answer two different questions and conflating them is what has kept this pipeline at zero promotions.
    //
    // PROMOTION asks "did this visit observe the region for long enough to decide anything", and the design specifies that gate as "the same condition the stock save already uses" - which is LLViewerRegion::mRegionTimer, the region's lifetime in the world (llviewerregion.cpp:902), not the agent's dwell in it. Measuring it as dwell meant a region the agent never entered had a stay of zero forever: on a fork with a 2048m draw distance, sixty-one of the owner's sixty-two tracked regions were neighbours, each streaming thousands of objects for the whole session, and not one of their records was ever offered to a gate. That is the "blocked stay 7004 ... immunity 0 persistence 0 score 0" line: every root fell out at the first hurdle and the other three never ran.
    //
    // SILENCE asks "was the viewer in a position to expect a confirmation", and that IS the agent's dwell: a neighbour region's interest list is distance-gated, so an object going unmentioned there is not evidence of anything and must not cost a record its paint order. The miss streak therefore keeps the dwell clock it always had.
    rs.mTrackedSecs = (rs.mTrackedStart > 0.0)
                    ? (U32)llclamp((F64)LLTimer::getElapsedSeconds() - rs.mTrackedStart, 0.0, 86400.0 * 30.0)
                    : 0;

    const bool settled       = (llmax(rs.mVisitSecs, rs.mTrackedSecs) >= settled_need);
    const bool agent_settled = (rs.mVisitSecs >= settled_need);
    const U32  visit_secs = (U32)llmax(1.f, (F32)visit_hours * (F32)SSROC_SECS_PER_HOUR);

    const size_t n = rs.mRecords.size();

    // Resolve linkset parentage from this visit's local ids. A child whose parent was never mentioned keeps its child flag and simply never promotes on its own - the alternative, treating it as a root, would score a parent-relative position as though it were a region position.
    for (size_t i = 0; i < n; ++i)
    {
        const U32 parent_local = rs.mLive[i].mParentLocalID;
        if (!parent_local) continue;

        rs.mRecords[i].mRecordFlags |= SSROC_REC_IS_CHILD;

        auto found = rs.mByLocalID.find(parent_local);
        if (found != rs.mByLocalID.end() && found->second != (U32)i)
        {
            rs.mRecords[i].mParentFullID = rs.mRecords[found->second].mFullID;
        }
    }

    // Child counts are derived from the ledger, not from live objects. The design's "captured from the live object at save" is unreachable at quit: LLWorld::resetClass runs gObjectList.destroy() before the removeRegion loop (llworld.cpp:136-142), so the last region of every session would report zero while a mid-session teleport reported the truth.
    for (size_t i = 0; i < n; ++i) rs.mRecords[i].mChildCount = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const LLUUID& parent = rs.mRecords[i].mParentFullID;
        if (parent.isNull()) continue;
        auto found = rs.mByFullID.find(parent);
        if (found != rs.mByFullID.end() && rs.mRecords[found->second].mChildCount < 65535)
        {
            ++rs.mRecords[found->second].mChildCount;
        }
    }

    // Silence does nothing but bump the miss streak, and only when the visit was long enough for silence to mean anything. It never touches the day bitmap, the score, immunity or promotion state.
    for (size_t i = 0; i < n; ++i)
    {
        if (rs.mLive[i].mNoted)
        {
            rs.mRecords[i].mMissStreak = 0;
        }
        else if (agent_settled)
        {
            rs.mRecords[i].mMissStreak = ssBumpU16(rs.mRecords[i].mMissStreak);
        }
    }

    // Fan the visit's dwell out once, here, rather than touching every record every frame.
    if (rs.mVisitSecs > 0)
    {
        for (size_t i = 0; i < n; ++i)
        {
            if (rs.mLive[i].mNoted) rs.mRecords[i].addDwell(rs.mVisitSecs);
        }
    }

    // The three hurdles, in order. Immunity is a hard gate evaluated before any score: a penalty would be fungible and could be bought back by unrelated terms, which is exactly wrong for a safety gate. The chain itself is ssROCPromoteVerdict, a pure function over the record and these thresholds, so it can be tested offline; everything that needs the live region is resolved here first.
    std::vector<U32> promoted_roots;

    SSROCPromoteGates gates;
    gates.mSettled       = settled;
    gates.mProofRequired = proof_required;
    gates.mVisitSecs     = visit_secs;
    gates.mVisitsNeed    = visits_need;
    gates.mDaysNeed      = days_need;
    gates.mScoreNeed     = score_need;

    SSROCPromoteTally tally;

    for (size_t i = 0; i < n; ++i)
    {
        SSROCRecord& rec = rs.mRecords[i];

        // The owner memo usually has an answer by now even when it did not during the visit, because the name cache batch has had the whole stay to land.
        if (!(rec.mRecordFlags & SSROC_REC_OWNER_PUBLIC_WORKS) && rec.mOwnerID.notNull())
        {
            auto memo = mOwnerClass.find(rec.mOwnerID);
            if (memo != mOwnerClass.end() && memo->second == OWNER_PUBLIC_WORKS)
            {
                rec.mRecordFlags |= SSROC_REC_OWNER_PUBLIC_WORKS;
            }
        }

        rec.mScore = scoreRecord(rec);

        // Immunity is the one hurdle that needs the live region, so it is resolved here and handed to the verdict. It is evaluated only where the verdict will actually consult it - a settled visit, a linkset root, a blob, no hard disqualifier - so the flags below are never written from a record the gate was never asked about.
        const bool root_candidate = !(rec.mRecordFlags & SSROC_REC_DISQUALIFIED) && !rec.mDP.empty() && rec.isRoot();
        gates.mImmune = false;

        if (root_candidate && settled)
        {
            gates.mImmune = hasImmunity(regionp, rs, rec);
            if (gates.mImmune)
            {
                rec.mRecordFlags |=  SSROC_REC_AUTORETURN_PROOF;
                rec.mRecordFlags &= ~SSROC_REC_AUTORETURN_UNKNOWN;
            }
            else
            {
                rec.mRecordFlags &= ~SSROC_REC_AUTORETURN_PROOF;
                rec.mRecordFlags |=  SSROC_REC_AUTORETURN_UNKNOWN;
            }
        }

        const U8 verdict = ssROCPromoteVerdict(rec, gates, &tally);

        if (verdict == SSROC_BLOCKED_CHILD)
        {
            // Decided in the second pass, once every root's verdict is known.
            rec.mBlockedBy = SSROC_BLOCKED_CHILD;
            continue;
        }

        if (verdict == SSROC_BLOCKED_STAY)
        {
            // Nothing was decided this visit, so an existing verdict is left exactly as it was - a promotion earned on an earlier visit is not overturned by a flyover.
            if (!rec.isPromoted()) rec.mBlockedBy = SSROC_BLOCKED_STAY;
            continue;
        }

        if (verdict != SSROC_BLOCKED_NONE)
        {
            rec.mRecordFlags &= ~SSROC_REC_PROMOTED;
            rec.mBlockedBy = verdict;
            continue;
        }

        rec.mRecordFlags |= SSROC_REC_PROMOTED;
        rec.mBlockedBy = SSROC_BLOCKED_NONE;
        ++rs.mPromotedThisVisit;   // counted so the exit path can skip re-mining the manifest when this visit promoted nothing
        promoted_roots.push_back((U32)i);
    }

    // Capacity. A truncation here is a statement about how much this region may cost, never a claim that the object is gone, so the losers keep every day and every confirmation they earned.
    if (settled && promoted_roots.size() > ghost_cap)
    {
        std::sort(promoted_roots.begin(), promoted_roots.end(),
                  [&rs](U32 a, U32 b)
                  {
                      const SSROCRecord& ra = rs.mRecords[a];
                      const SSROCRecord& rb = rs.mRecords[b];
                      const U32 da = ra.distinctDaysSeen();
                      const U32 db = rb.distinctDaysSeen();
                      if (da != db) return da > db;   // tenure outranks score, so a dense region cannot truncate away its aging candidates
                      return ra.mScore > rb.mScore;
                  });

        for (size_t k = ghost_cap; k < promoted_roots.size(); ++k)
        {
            SSROCRecord& rec = rs.mRecords[promoted_roots[k]];
            rec.mRecordFlags &= ~SSROC_REC_PROMOTED;
            rec.mBlockedBy = SSROC_BLOCKED_CAPACITY;
        }
        promoted_roots.resize(ghost_cap);
    }

    // Children ride their root, in both directions. The child bucket is by far the largest in the exit histogram and it is a CONSEQUENCE rather than a gate - promotion is a property of the linkset root by design, because a child's cached position is parent-relative and it is a fragment with nowhere to stand on its own. Counted apart from that, though, is the one child failure that IS a defect: a child whose root is not in the record set at all, which no amount of root promotion can ever rescue.
    U32 children = 0;
    U32 children_orphaned = 0;
    U32 children_promoted = 0;

    for (size_t i = 0; i < n; ++i)
    {
        SSROCRecord& rec = rs.mRecords[i];
        if (rec.isRoot()) continue;
        if (rec.mRecordFlags & (SSROC_REC_DISQUALIFIED)) continue;
        if (rec.mDP.empty()) continue;

        ++children;

        bool root_promoted = false;
        bool root_known    = false;
        if (rec.mParentFullID.notNull())
        {
            auto found = rs.mByFullID.find(rec.mParentFullID);
            if (found != rs.mByFullID.end())
            {
                root_known    = true;
                root_promoted = rs.mRecords[found->second].isPromoted();
            }
        }

        if (!root_known) ++children_orphaned;

        if (root_promoted)
        {
            if (!rec.isPromoted()) ++rs.mPromotedThisVisit;
            rec.mRecordFlags |= SSROC_REC_PROMOTED;
            rec.mBlockedBy = SSROC_BLOCKED_NONE;
            ++children_promoted;
        }
        else if (settled)
        {
            rec.mRecordFlags &= ~SSROC_REC_PROMOTED;
            rec.mBlockedBy = SSROC_BLOCKED_CHILD;
        }
    }

    // <SS:Nexii> What the gates DID, not merely what they rejected. Without this a blocked-reason histogram reading "immunity 0 persistence 0 score 0" is unreadable: it looks exactly the same whether those gates passed everything or were never reached at all, and it was read the wrong way for several sessions. Emitted after the child pass so the largest bucket in the histogram is reported as a consequence with a cause beside it rather than as an unexplained majority.
    LL_INFOS("SSROC") << "Promotion gates for region " << rs.mHandle
                      << ": stay " << (settled ? "passed" : "blocked")
                      << " (agent " << rs.mVisitSecs << "s, region " << rs.mTrackedSecs << "s, need " << settled_need << "s)"
                      << " | roots " << tally.mRoots
                      << ", reached immunity " << tally.mRootsSettled
                      << " (immune " << tally.mImmune << ")"
                      << ", reached persistence " << tally.mReachedPersistence
                      << ", reached score " << tally.mReachedScore
                      << ", promoted " << tally.mPromoted
                      << " | children " << children << ", inherited " << children_promoted
                      << ", rootless " << children_orphaned
                      << " | best days " << tally.mBestDays << "/" << days_need
                      << ", best visits " << tally.mBestVisits << "/" << visits_need
                      << ", best score " << tally.mBestScore << "/" << score_need << LL_ENDL;
}

bool SSROCLedger::onRegionRemoved(LLViewerRegion* regionp, SSROCRegionFile& file)
{
    if (!regionp || !SSROCStore::enabled()) return false;

    const U64 handle = regionp->getHandle();

    RegionState* rsp = stateFor(handle);
    if (!rsp) return false;

    RegionState& rs = *rsp;

    // Close the dwell clock for this region before anything reads it. gAgent::cleanup nulls mRegionp without firing the region-changed signal (llagent.cpp:610), so the running segment at quit can only be closed from the stored handle.
    if (mDwellHandle == handle) flushDwell();

    // Sandbox regions keep their aux data - terrain is not affected by object return and the win is real for anyone who sandboxes - but never their objects, including any a previous non-sandbox visit wrote.
    if (rs.mSandbox || regionp->getRegionFlag(REGION_FLAGS_SANDBOX))
    {
        const bool had = !file.mRecords.empty();
        file.mRecords.clear();
        file.mManifest.clear();
        LL_INFOS("SSROC") << "Region " << handle << " is a sandbox: dropped "
                          << (had ? "the stored object ledger" : "nothing") << LL_ENDL;
        mRegions.erase(handle);
        return had;
    }

    if (rs.mRecords.empty())
    {
        mRegions.erase(handle);
        return false;
    }

    runPromotion(regionp, rs);

    // Merge by FullID rather than replacing. When the async load landed the ledger already holds everything the file did, so this is an in-place update; when it did not, the union is what stops a slow disk read from erasing a region's history.
    std::unordered_map<LLUUID, size_t> by_id;
    by_id.reserve(file.mRecords.size());
    for (size_t i = 0; i < file.mRecords.size(); ++i)
    {
        if (file.mRecords[i].mFullID.notNull()) by_id[file.mRecords[i].mFullID] = i;
    }

    // Moved rather than copied: every record owns a heap blob, rs is erased immediately after this, and all of it lands on the teleport frame where several neighbouring regions exit at once.
    file.mRecords.reserve(file.mRecords.size() + rs.mRecords.size());

    for (SSROCRecord& rec : rs.mRecords)
    {
        auto found = by_id.find(rec.mFullID);
        if (found == by_id.end())
        {
            by_id[rec.mFullID] = file.mRecords.size();
            file.mRecords.push_back(std::move(rec));
        }
        else
        {
            file.mRecords[found->second] = std::move(rec);
        }
    }

    // Record cap. Promoted and long-tenured records are kept; the rest are a speculative tail that must not grow without bound, and dropping one is a capacity decision that costs a re-observation, not correctness.
    static LLCachedControl<U32> max_records(gSavedSettings, "SSROCMaxRecordsPerRegion", 8000);
    const size_t record_cap = (size_t)(U32)max_records;
    if (record_cap > 0 && file.mRecords.size() > record_cap)
    {
        // distinctDaysSeen is a popcount over sixteen bytes and the comparator called it twice per comparison, roughly 2n*log2(n) times across the sort. Computing it once per record into a key and sorting those instead makes it n calls, and the records themselves are then moved rather than shuffled.
        struct SortKey
        {
            U32  mIndex;
            U32  mDays;
            F32  mScore;
            U16  mConfirms;
            bool mPromoted;
        };

        const size_t original = file.mRecords.size();
        std::vector<SortKey> keys;
        keys.reserve(original);
        for (size_t i = 0; i < original; ++i)
        {
            const SSROCRecord& r = file.mRecords[i];
            SortKey k;
            k.mIndex    = (U32)i;
            k.mDays     = r.distinctDaysSeen();
            k.mScore    = r.mScore;
            k.mConfirms = r.mConfirmCount;
            k.mPromoted = r.isPromoted();
            keys.push_back(k);
        }

        std::sort(keys.begin(), keys.end(),
                  [](const SortKey& a, const SortKey& b)
                  {
                      if (a.mPromoted != b.mPromoted) return a.mPromoted;
                      if (a.mDays != b.mDays) return a.mDays > b.mDays;
                      if (a.mScore != b.mScore) return a.mScore > b.mScore;
                      return a.mConfirms > b.mConfirms;
                  });

        std::vector<SSROCRecord> kept;
        kept.reserve(record_cap);
        for (size_t i = 0; i < record_cap; ++i)
        {
            kept.push_back(std::move(file.mRecords[keys[i].mIndex]));
        }
        file.mRecords.swap(kept);

        LL_INFOS("SSROC") << "Region " << handle << ": trimmed ledger from " << original
                          << " to " << record_cap << " records" << LL_ENDL;
    }

    // [MANIFEST] is fed from promoted blobs only, and is only rebuilt when this visit actually promoted something or the file has no manifest yet. Without that guard a ten second flyover past a region whose file already holds thousands of promoted records re-mines every one of them from scratch for no gain, on the same frame several neighbours are exiting.
    if (rs.mPromotedThisVisit > 0 || file.mManifest.empty())
    {
        std::vector<LLUUID> mined;
        for (const SSROCRecord& rec : file.mRecords)
        {
            if (rec.isPromoted()) ssROCMineAssets(rec.mDP, mined);
        }
        std::sort(mined.begin(), mined.end());
        mined.erase(std::unique(mined.begin(), mined.end()), mined.end());
        file.mManifest.swap(mined);
    }

    // <SS:Nexii/> The set-to-group answers, taken back out of the shared cache and stored with this region. Only for records the file actually holds: the shared cache belongs to the viewer, this file belongs to the object cache, and an answer about an object it is not remembering is not its to keep. Written unconditionally rather than only on a promoting visit because, unlike [MANIFEST], it is not derived from the records - it is the only copy of something that cost a message.
    file.mGroups.clear();
    file.mGroups.reserve(file.mRecords.size());
    for (const SSROCRecord& stored : file.mRecords)
    {
        SSObjectFacts obj;
        if (!ssObjectFactsGet(handle, stored.mFullID, obj)) continue;
        if (!(obj.mHave & SS_OBJFACTS_PROPERTIES)) continue;
        file.mGroups.push_back(std::make_pair(stored.mFullID, obj.mGroupID));
    }

    // One line per region, and it is the whole field diagnostic for Phase 2a: without the reason histogram a three-hurdle gate cannot be debugged from a user's log.
    U32 blocked[SSROC_BLOCKED_COUNT] = { 0 };
    U32 promoted = 0;
    U32 null_owner = 0;
    U32 immune = 0;

    // How many records are recent enough to be drawn again on a return visit without having earned a place in the long term cache. Region contents rarely change over a few hours, so this is the population that makes region hopping pay off immediately - and counting it here measures the idea before any ghost rendering exists to consume it.
    static LLCachedControl<U32> recent_minutes(gSavedSettings, "SSROCRecentGhostMinutes", 180);
    const U64 recent_cutoff = ((U32)recent_minutes > 0 && (U64)time(NULL) > (U64)(U32)recent_minutes * 60)
                            ? (U64)time(NULL) - (U64)(U32)recent_minutes * 60
                            : 0;
    U32 recent = 0;
    U32 stale  = 0;

    // How close the promotion pipeline actually is to producing anything. "promoted 0" on its own is unreadable - it looks identical whether the gate is one day away or structurally unreachable - so the best day count any record in this region has reached is reported beside it, since distinct days is the only currency on the unproven path and the only path open on land the user does not own.
    U32 best_days = 0;
    U32 nearly    = 0;

    static LLCachedControl<U32> promote_days_log(gSavedSettings, "SSROCPromoteDaysUnproven", 3);
    const U32 days_need_log = (U32)promote_days_log;

    for (const SSROCRecord& rec : file.mRecords)
    {
        if (rec.isPromoted()) ++promoted;
        if (rec.mOwnerID.isNull()) ++null_owner;
        if (rec.mRecordFlags & SSROC_REC_AUTORETURN_PROOF) ++immune;
        if (rec.mRecordFlags & SSROC_REC_ID_STALE) ++stale;
        if (recent_cutoff && rec.mLastConfirmed >= recent_cutoff && rec.mBlockedBy != SSROC_BLOCKED_DISQUALIFIED) ++recent;
        if (rec.mBlockedBy < SSROC_BLOCKED_COUNT) ++blocked[rec.mBlockedBy];

        const U32 days = rec.distinctDaysSeen();
        if (days > best_days) best_days = days;
        // Only records on the CONSERVATIVE path are counted down in days: one blocked on persistence while immunity is established is short of VISITS, not days, and reporting it here would say it was a day away when another visit is what it needs.
        if (rec.mBlockedBy == SSROC_BLOCKED_PERSISTENCE
            && !(rec.mRecordFlags & SSROC_REC_AUTORETURN_PROOF)
            && days + 1 >= days_need_log) ++nearly;
    }
    mMetrics.mPromoted += promoted;

    LL_INFOS("SSROC") << "Ledger for region " << handle
                      << ": " << file.mRecords.size() << " records, " << rs.mConfirmed << " confirmed this visit"
                      << ", " << rs.mCreated << " new, " << rs.mNoBlob << " blobless"
                      << " | visit " << rs.mVisitSecs << "s agent, " << rs.mTrackedSecs << "s in world"
                      << " | promoted " << promoted << ", recent " << recent << ", immune " << immune
                      << ", null-owner " << null_owner
                      << " | blocked stay " << blocked[SSROC_BLOCKED_STAY]
                      << " disq " << blocked[SSROC_BLOCKED_DISQUALIFIED]
                      << " immunity " << blocked[SSROC_BLOCKED_IMMUNITY]
                      << " persistence " << blocked[SSROC_BLOCKED_PERSISTENCE]
                      << " score " << blocked[SSROC_BLOCKED_SCORE]
                      << " child " << blocked[SSROC_BLOCKED_CHILD]
                      << " noblob " << blocked[SSROC_BLOCKED_NOBLOB]
                      << " capacity " << blocked[SSROC_BLOCKED_CAPACITY]
                      << " | best day count " << best_days << " of " << days_need_log
                      << ", " << nearly << " records one day short"
                      << " | stale ids " << stale
                      << " | manifest " << file.mManifest.size() << LL_ENDL;

    mRegions.erase(handle);
    return true;
}

// Landscape or mover, from the ledger's history: the same evidence promotion weighs, read without scoring.
bool SSROCLedger::restVerdict(U64 handle, const LLUUID& full_id, bool& out_static) const
{
    auto rit = mRegions.find(handle);
    if (rit == mRegions.end()) return false;
    const RegionState& rs = rit->second;
    auto it = rs.mByFullID.find(full_id);
    if (it == rs.mByFullID.end() || it->second >= rs.mRecords.size()) return false;
    const SSROCRecord& rec = rs.mRecords[it->second];
    // <SS:Nexii> The bit is shared by motion evidence and the blob-size cap (ssROCIsDisqualified), and only motion is a mover verdict: an oversized record never keeps a blob, so a blobless disqualified record falls through to watching instead of excluding a large static building from the census for ever. A physical mover salvaged across a format bump (ssroccache.cpp salvage clears mDP and keeps the bit) lands here too and is watched rather than condemned: salvage also zeroed its flags and score, so there is no motion evidence left to condemn it with. [interaction: SSWorldFieldShapes::trackRest]
    if (rec.mRecordFlags & SSROC_REC_DISQUALIFIED)
    {
        if (rec.mDP.empty()) return false;
        out_static = false;
        return true;
    }
    // A move on record is not a verdict: a building the owner shifted once would otherwise stay a mover for ever.
    // It only withholds the landscape credit, so the census watches it settle instead.
    if (rec.mMoveCount > 0) return false;
    if (rec.isPromoted() || rec.mEntryCount >= 2) { out_static = true; return true; }
    return false;
}

std::string SSROCLedger::metricsString() const
{
    return llformat("ROC ledger: regions %u (sandbox %u) | sightings %u, records %u, blobless %u | promoted %u | owner lookups %u",
                    mMetrics.mRegionsTracked, mMetrics.mRegionsSandbox,
                    mMetrics.mSightings, mMetrics.mRecordsCreated, mMetrics.mNoBlob,
                    mMetrics.mPromoted, mMetrics.mOwnerLookups);
}
