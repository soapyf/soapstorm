/**
 * @file ssroccache.h
 * @brief Region Object Cache (ROC) container and store - see doc/region_object_cache.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_ROCCACHE_H
#define SS_ROCCACHE_H

#include "llsingleton.h"
#include "lluuid.h"

#include <utility>
#include "v3math.h"
#include <boost/signals2.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// The ROC is a FullID-keyed sidecar store layered over an untouched LLVOCache/.slc - it never reads or writes the protocol cache, and its own format is versioned per file so it can iterate without inflicting a global object-cache wipe on users. Rationale and the full design live in doc/region_object_cache.md.

constexpr U32 SSROC_MAGIC             = 0x434f5253; // 'SROC' as stored little-endian
// <SS:Nexii> 6 -> 7 for a SEMANTIC change rather than a layout one, which is a reason to bump this that the line below does not cover and is worth stating so nobody "optimises" the bump away.
//
// Version 6 files predate the epoch flags. A record in one carries neither ID_CURRENT nor ID_STALE, and absent-both reads as "not stale", so every such record is injectable. That is wrong for a specific and reachable file: one saved by a build with no epoch logic, on a visit that saw a restart, which therefore stamped the NEW CacheID into the header while its records still held ids from before it. On the next visit the CacheID compares equal, mode A passes, nothing is marked stale, and remembered ids from a dead epoch get painted - the exact case the epoch flags exist to refuse.
//
// Discarding is right rather than merely easy here. There is no in-file evidence that separates a safe version 6 file from that one, so a migration would have to guess, and ROC data is derived: the cost is that ghosts stay quiet for one visit per region while the ledger records them again.
// <SS:Nexii> 7 -> 8 for the same reason the last bump happened, applied to a surface where the consequence is larger. Version 7 records may carry NEITHER epoch bit, which reads as "not stale" and therefore as injectable. That was survivable while the only consumer was ghost painting, where a wrong guess shows the user a stale object for ninety seconds. It is not survivable now that the same record set answers the simulator's ObjectUpdateCached probes: a record whose stored local id belongs to a dead epoch would claim a CRC match for an object that is no longer there, and the viewer would then never request the object that actually is. Discarding is right rather than merely easy, exactly as before - nothing in a version 7 file separates a safe record from that one, and ROC data is derived, so the cost is one cold visit per region while the ledger records it again.
constexpr U32 SSROC_FORMAT_VERSION    = 8;         // bump on ANY layout change - files from an older version are rejected outright as DATA, never migrated, but see SSROC_MIN_SALVAGE_VERSION below for the one thing that is carried across
// <SS:Nexii> The oldest version whose [OBJECTS] record layout is byte-identical to this one, and therefore the oldest file whose TENURE can be read back after a bump. Both bumps so far were semantic rather than structural - verified field by field against the version 7 serialiser in commit cc6a9040d6 - so a version 6 or 7 file parses exactly, and everything either bump was actually about is thrown away by ssROCStripToTenure rather than trusted.
//
// This exists because discarding the whole file was costing the feature the one thing it cannot rebuild. The promotion pipeline's conservative path needs a record confirmed on SSROCPromoteDaysUnproven DISTINCT CALENDAR DAYS, and calendar days can only be earned by waiting - so a format bump does not cost "one cold visit per region", it resets every record on the grid to day one and pushes promotion three more days into the future. The format moved twice in one week and the owner's ledger has therefore never held a record older than a day, which is exactly why nothing has ever promoted.
//
// Salvage is safe precisely because it keeps the complement of what the bumps were about. Both bumps concerned the meaning of a stored LOCAL ID against a simulator cache epoch; tenure is FullID-keyed presence history that no epoch can invalidate. The blob, the local id, the CRC, the update flags, the score, the promotion flag and both epoch bits are dropped, so a salvaged record can be neither painted (ssROCBuildInjectPlan refuses an empty blob) nor offered to the protocol cache (ssROCBuildSeedPlan refuses it as SSROC_SEED_NO_BLOB), and the file's own CacheID is nulled so the visit reads as SSROC_BACK_EPOCH_UNKNOWN. It carries history and nothing that could stand an object anywhere.
//
// RAISE THIS to SSROC_FORMAT_VERSION on any bump that genuinely moves the record layout - salvage reads the old bytes with the current reader and has no other way to know.
constexpr U32 SSROC_MIN_SALVAGE_VERSION = 6;
constexpr U32 SSROC_DAY_BITMAP_BYTES  = 16;         // 128 days of presence bits, one bit per calendar day, bit 0 = the day of mLastConfirmed
constexpr U32 SSROC_MAX_DP_SIZE       = 10000;      // matches MAX_ENTRY_BODY_SIZE in llvocache.cpp - blobs larger than this cannot be cached by the stock path either
// Sized for a fully upgraded region: 30000 objects at a few hundred bytes of blob each lands around 20 MB, and a region full of unusually large blobs has to fit too or saveRegionAsync silently refuses the whole file and the region never caches at all. The global budget in SSROCDiskBudgetMB is what actually bounds disk use; these two only bound one region.
constexpr U32 SSROC_SOFT_REGION_BYTES = 24 * 1024 * 1024;
constexpr U32 SSROC_HARD_REGION_BYTES = 96 * 1024 * 1024;

// Section identifiers for the file's section table. Unknown sections are skipped on read so a newer viewer's extra sections never break an older one within the same format version.
enum ESSROCSection : U32
{
    SSROC_SECTION_OBJECTS  = 1,
    SSROC_SECTION_AUX      = 2,
    SSROC_SECTION_MANIFEST = 3,
    SSROC_SECTION_GLTF     = 4,
    SSROC_SECTION_GROUPS   = 5,   // <SS:Nexii/> Stage C's set-to-group answers. A new SECTION rather than a new record field on purpose: a record layout change would have to raise SSROC_MIN_SALVAGE_VERSION, which resets every record on the grid to day one and pushes the conservative promotion path three more days out. Unknown sections are skipped within a version, so an older build reads this file and simply ignores this.
};

// Per-record state bits. Everything here is ROC's own bookkeeping - none of it is protocol state and none of it is ever written back into the .slc.
enum ESSROCRecordFlags : U32
{
    SSROC_REC_AUTORETURN_PROOF   = 1 << 0,  // established immune to parcel auto-return (owner or group match, or auto-return disabled on the parcel)
    SSROC_REC_AUTORETURN_UNKNOWN = 1 << 1,  // immunity could not be established this session - promotion is penalised, not blocked
    SSROC_REC_BRIDGE_VERIFIED    = 1 << 2,  // an answered bridge sweep confirmed existence at least once
    SSROC_REC_REZTIME_VERIFIED   = 1 << 3,  // rez age passed the promotion threshold via an answered bridge sweep
    SSROC_REC_GROUP_SET          = 1 << 4,  // object is set to a group (only knowable from ObjectProperties or the bridge)
    SSROC_REC_DIRTY              = 1 << 5,  // mutated since it was written - re-snapshot the blob at save
    SSROC_REC_PROMOTED           = 1 << 6,  // passed every promotion gate and may be rezzed as a ghost on entry
    SSROC_REC_OWNER_PUBLIC_WORKS = 1 << 7,  // owner resolved to a Mole (Linden Department of Public Works) - permanent landscape furniture by construction
    SSROC_REC_DISQUALIFIED       = 1 << 8,  // tripped a hard disqualifier (physics, character, avatar PCode, attachment state, oversized blob) - never cleared, an object that was physical once is not trusted to have stopped
    SSROC_REC_IS_CHILD           = 1 << 9,  // the blob carried a non-zero ParentID, so its Pos is parent-relative and it may never be classified or positioned on its own

    // Local-id epoch. The simulator's CacheID names one local-id assignment epoch, and it is stored ONCE per file beside every record - so after a visit whose CacheID changed, the file carries the NEW CacheID next to records whose stored ids are still from the OLD one, because only the records the simulator actually mentioned that visit were re-keyed. Without these two bits the visit after that reads as mode A and would file a remembered object under a dead id, which is the one thing the whole design forbids. Neither bit set means a file written before this existed: legacy, treated as usable, so the format version does not move.
    SSROC_REC_ID_CURRENT         = 1 << 10, // the stored mLastLocalID came from the simulator's own stream under the CacheID this file was last saved with
    SSROC_REC_ID_STALE           = 1 << 11, // the CacheID changed while this record went unmentioned, so its stored local id names whatever the simulator has since put there - never inject it
};

// The reserved private local-id range. Nothing in the protocol reserves it; it is chosen from measurement - 556,931 real local ids across 43 SL regions, sampled from both the .slc and .roc stores on this machine, peak 0x399791FA, and not one value at or above 0x40000000. The floor therefore sits over three billion ids above the highest counter any region here has ever reached. Today nothing allocates in it; the constant exists so "the ROC never causes a send" is a one-instruction test that a stock guard can enforce rather than an invariant that has to be audited by reading call sites. See doc/region_object_cache.md.
constexpr U32 SSROC_SYNTH_ID_FLOOR = 0xFF000001u;

// <SS:Nexii> Moved here from ssrocledger.h so the promotion decision can be a pure function over a record and its thresholds, testable offline against the same store the viewer uses - exactly as ssROCBuildSeedPlan and ssROCBuildInjectPlan already are. The ledger owns WHEN the decision runs and what the region contributes to it; this header owns the decision itself.
//
// Why a record could not be promoted. Stored as one byte on every non-promoted record so the region-exit histogram is inspectable in the field - a three-hurdle gate with no reason code is undebuggable, and a gate that never RAN prints identically to one that always passes unless the tally below is reported beside it.
enum ESSROCBlockedBy : U8
{
    SSROC_BLOCKED_NONE         = 0,  // promoted
    SSROC_BLOCKED_STAY         = 1,  // the visit was too short for scoring to decide anything (SSROCSettledStaySecs)
    SSROC_BLOCKED_DISQUALIFIED = 2,  // hard disqualifier: physics, character, avatar PCode, attachment state, oversized blob
    SSROC_BLOCKED_IMMUNITY     = 3,  // auto-return immunity was required and could not be established
    SSROC_BLOCKED_PERSISTENCE  = 4,  // not enough visits (immune) or distinct days (unproven) yet
    SSROC_BLOCKED_SCORE        = 5,  // cleared both gates but scored below SSROCPromoteScore
    SSROC_BLOCKED_CHILD        = 6,  // linkset child whose root was not promoted, or whose root was never seen
    SSROC_BLOCKED_NOBLOB       = 7,  // never carried a usable DP blob, so it can never be rezzed
    SSROC_BLOCKED_CAPACITY     = 8,  // score-ranked out at the per-region cap - a capacity decision, never an existence claim
    SSROC_BLOCKED_COUNT        = 9,
};

const char* ssROCBlockedByName(U8 blocked);

// One cached object. The DP blob is byte-identical to the ObjectUpdateCompressed payload the .slc stores, so every existing static unpacker and the whole OUT_FULL_CACHED rez path can consume it unmodified.
struct SSROCRecord
{
    SSROCRecord();

    LLUUID      mFullID;
    LLUUID      mParentFullID;      // null when this record is a linkset root
    LLUUID      mOwnerID;           // read out of the DP blob at parse time - free, offline, no probe required
    U32         mLastLocalID;
    U32         mCRC;
    U32         mUpdateFlags;       // captured live at message time - NOT present in the .slc disk format
    U32         mRecordFlags;       // ESSROCRecordFlags
    F32         mScore;
    U64         mFirstSeen;         // unix seconds
    U64         mLastConfirmed;     // unix seconds
    U64         mRezTimeEpoch;      // 0 when unknown - the viewer protocol has no rez time, this only ever comes from the bridge
    U16         mSessionsSeen;
    U16         mCRCChangeCount;
    U16         mChildCount;        // captured from the live object at save - root blobs carry no child enumeration, so linkset completeness is undecidable without it
    U16         mEntryCount;        // distinct region entries during which this object was confirmed
    U32         mDwellSecs;         // total seconds the agent was present in the region with this object confirmed, across all sessions
    U16         mConfirmCount;      // confirmations across all sessions - the ROC-side equivalent of the .slc hit and dupe counters, which die with the .slc and are unreachable from here anyway
    U16         mMoveCount;         // times the object was re-sighted outside its own neighbourhood
    U16         mMissStreak;        // consecutive settled visits with no confirmation - what silence does, and its complete effect
    U8          mBlockedBy;         // ESSROCBlockedBy - zero when promoted
    U8          mDayBitmap[SSROC_DAY_BITMAP_BYTES];
    LLVector3   mPos;
    LLVector3   mScale;
    std::vector<U8> mDP;

    // Rootness is decided by the blob's ParentID, not by whether the parent's FullID was resolvable. A child whose parent was never mentioned this visit still has a parent-relative position, and treating it as a root would file it at the region origin.
    bool isRoot() const { return (mRecordFlags & SSROC_REC_IS_CHILD) == 0; }
    bool isPromoted() const { return (mRecordFlags & SSROC_REC_PROMOTED) != 0; }

    // Day-bitmap helpers. Distinct calendar days are the CONSERVATIVE currency, used when nothing has established that the object is safe from parcel auto-return - there, only the passage of real days is evidence.
    void markSeenOn(U64 unix_secs);
    U32  distinctDaysSeen() const;
    U32  tenureDays() const;

    // Dwell accounting. A long stay is genuinely more evidence than a glance, so every visit_secs of confirmed presence counts as another visit on top of the entry itself - an evening in your home region is worth several drive-bys past a stranger's build.
    void addDwell(U32 secs);
    void noteEntry();
    U32  visitCount(U32 visit_secs) const;
};

// <SS:Nexii> The three-hurdle promotion decision, lifted out of SSROCLedger::runPromotion so it is a pure function of one record and one set of thresholds. The ledger still owns everything that needs a live region - the settled-stay measurement, the auto-return immunity ladder, the score - and hands the answers in here.
//
// Everything the gates need, resolved by the caller before the record is offered. Both booleans are decisions the ledger has already made against the live region, so this function never has to reach for one.
struct SSROCPromoteGates
{
    SSROCPromoteGates();

    bool mSettled;          // this visit observed the region for long enough to decide anything at all
    bool mImmune;           // auto-return immunity was established for THIS record, evaluated by the ledger against the parcel overlay
    bool mProofRequired;    // SSROCRequireAutoReturnProof: refuse the conservative distinct-days path entirely
    U32  mVisitSecs;        // seconds of confirmed presence that count as one additional visit (SSROCVisitHours)
    U32  mVisitsNeed;       // SSROCPromoteVisits, the currency where immunity is established
    U32  mDaysNeed;         // SSROCPromoteDaysUnproven, the currency where it is not
    F32  mScoreNeed;        // SSROCPromoteScore
};

// What the gates actually did, not merely what they rejected. A gate that never ran and a gate that always passes produce an identical zero in a blocked-reason histogram, which is precisely how "immunity 0 persistence 0 score 0" was read for several sessions as "those gates are fine" when in truth no record had ever reached them.
struct SSROCPromoteTally
{
    SSROCPromoteTally();

    U32 mConsidered;        // records offered to the verdict
    U32 mRoots;             // of those, linkset roots carrying a blob and no hard disqualifier - the only records the gates apply to
    U32 mRootsSettled;      // roots that cleared the stay gate, so the immunity test genuinely RAN for them
    U32 mImmune;            // of those, how many established immunity
    U32 mReachedPersistence;// roots the persistence gate was actually evaluated for
    U32 mReachedScore;      // roots the score comparison was actually evaluated for
    U32 mPromoted;
    U32 mBestDays;          // the highest distinct-day count any root reached, so "promoted 0" reads as a countdown
    U32 mBestVisits;        // and the same for the immune path's currency
    F32 mBestScore;         // the best score among roots that got as far as the score gate, 0 when none did
};

// Returns an ESSROCBlockedBy; SSROC_BLOCKED_NONE means promote. Order matters and mirrors the design: hard disqualifier, then blob, then rootness, then the stay gate, then immunity, persistence and score. `tally` may be null.
U8 ssROCPromoteVerdict(const SSROCRecord& rec, const SSROCPromoteGates& gates, SSROCPromoteTally* tally);

// Sandbox policy. Auto-return is a PER-PARCEL setting, so immunity is always decided per parcel - but two cases are decided up front.
//
// A region carrying REGION_FLAGS_SANDBOX is authoritative (only Linden Lab can set it) and its object content is transient by definition, so its objects are never recorded at all. Region flags arrive in unpackRegionHandshake (llviewerregion.cpp:3276) and can change mid-session via the RegionInfo message (llviewermessage.cpp:5380), so this is re-checked rather than latched.
//
// A parcel whose NAME contains "sandbox" is a heuristic, not a fact - anyone can name a parcel anything - so it does not skip recording. It only denies the structural immunity credit, dropping those objects onto the conservative distinct-days path where they must survive real days before being cached. A false positive therefore costs patience, never correctness.
bool ssROCParcelNameDeniesImmunity(const std::string& parcel_name);

// Linden Department of Public Works. Objects owned by a Mole are the most permanent content on the grid - roads, bridges, terraforming, public builds - placed deliberately by the estate owner's own department, so they earn both the auto-return immunity credit and a large score bonus.
//
// Takes a LEGACY name ("Abnor Mole"), not a username: LLAvatarName::getUserName() returns the dot-separated form ("Abnor.Mole"), so anything that splits a username on whitespace never sees a last name at all.
//
// Deliberately narrower than FSCommon::isLinden, which also matches Scout, Tester and ProductEngine accounts - those are contractor and QA identities with no connection to permanent public works.
//
// Moving public works (trains, vehicles) need no special case here: they are physical, and physical objects are rejected by a hard disqualifier that is evaluated BEFORE any score, so a bonus can never resurrect one.
bool ssROCOwnerIsPublicWorks(const std::string& legacy_name);

// Injection order for a region's cached objects. Painting a whole region takes many frames, so what arrives in the first of them decides whether the place reads as itself straight away or as a scatter of small props with the buildings missing.
//
// Two criteria, blended rather than tiered so neither can starve the other: how permanent the object looks, and how visually large it is. A tiered sort would put several thousand tiny promoted objects ahead of a large one that only qualifies on recency, which is the wrong picture for exactly the region-hopping case the recent tier exists to serve.
//
// Size is the object's own bounding radius plus a bonus for anchoring a large linkset, because a building's root prim is frequently small while the linkset hanging off it is not - and the root prim's scale is all a cached blob carries.
//
// Returns roughly 0..1; higher is painted first. `recent_cutoff` is the unix second before which a record counts only as recent rather than promoted; pass 0 to score purely on promotion.
F32 ssROCInjectPriority(const SSROCRecord& rec, U64 recent_cutoff);

// Which of a region's cached records may be painted back on arrival. The two tiers answer different questions and are gated independently: PROMOTED asks "is this permanent enough to remember for weeks", RECENT asks "was this here a moment ago", and the second is the commoner and stronger case when hopping between a handful of regions in one sitting.
struct SSROCInjectPolicy
{
    SSROCInjectPolicy();

    bool mPromotedTier;     // draw records that cleared the immunity, persistence and score gates
    bool mRecentTier;       // draw records confirmed at or after mRecentCutoff, whatever their long-term standing
    U64  mRecentCutoff;     // unix seconds; 0 means the recent tier contributes nothing regardless of mRecentTier
    U32  mMaxGhosts;        // hard ceiling on the plan, applied after ranking so the cut falls on the least valuable objects
};

constexpr S32 SSROC_TERRAIN_ASSETS = 4;   // mirrors LLTerrainMaterials::ASSET_COUNT and LLVLComposition::CORNER_COUNT

// Terrain composition parameters: the four detail assets plus the per-corner blend heights. Kept as a plain struct so it can be packed and unit tested without dragging the composition class in.
struct SSROCComposition
{
    SSROCComposition();

    bool    mValid;
    LLUUID  mDetail[SSROC_TERRAIN_ASSETS];
    F32     mStartHeight[SSROC_TERRAIN_ASSETS];
    F32     mHeightRange[SSROC_TERRAIN_ASSETS];
};

// Region-scoped aux data - terrain heightmap, water height and composition params. EEP region environment is deliberately NOT cached (owner decision 2026-08-27): it is the one aux item whose apply path fights the viewer's own sequencing (the region-change hook fires before the destination handshake exists to validate against) and it risks a visible double sky transition, for a win the rest of the cache does not depend on.
struct SSROCAux
{
    SSROCAux() : mWaterHeight(0.f), mHeightmapGridsPerEdge(0), mHeightmapGridsPerPatchEdge(0), mHeightmapMin(0.f), mHeightmapMax(0.f) {}

    F32                 mWaterHeight;
    U32                 mHeightmapGridsPerEdge;   // 0 when no heightmap is stored - var-regions and OpenSim large patches differ, so dimensions travel with the data
    U16                 mHeightmapGridsPerPatchEdge; // patch size at capture time - a region that changed patch layout must fall back to the LayerData stream rather than paint mismatched relief
    F32                 mHeightmapMin;            // quantisation range, stored so a restore is exact to within one 65535th of the region's own relief
    F32                 mHeightmapMax;
    std::vector<U16>    mHeightmap;               // row-major, mHeightmapGridsPerEdge squared
    SSROCComposition    mComposition;

    bool empty() const { return mHeightmap.empty() && !mComposition.mValid; }

    // Quantise a region's decoded surface Z into the store. `stride` is the source row stride in floats, which is not always equal to grids_per_edge. Returns false on degenerate input rather than storing something a restore would misinterpret.
    bool packHeightmap(const F32* src, U32 grids_per_edge, U32 stride);

    // Restore into a caller-owned float array. Fails if the cached dimensions do not match what the live region expects - a var-region that changed size must fall back to the LayerData stream rather than write mismatched relief.
    bool unpackHeightmap(F32* dst, U32 grids_per_edge, U32 stride) const;
};

// One region's whole cache file, in memory. Loads are parsed off-thread and handed over as a shared_ptr; saves take a main-thread snapshot and serialise off-thread.
struct SSROCRegionFile
{
    SSROCRegionFile();

    U64                         mRegionHandle;
    LLUUID                      mRegionID;
    LLUUID                      mLastCacheID;     // the sim's CacheID at the time of writing - a mismatch on the next visit means the sim restarted and local IDs are worthless
    U64                         mSavedAt;         // unix seconds
    U32                         mSessionsSeen;
    U32                         mFlags;
    std::vector<SSROCRecord>    mRecords;
    SSROCAux                    mAux;
    std::vector<LLUUID>         mManifest;        // deduped asset UUIDs mined from the records, exported to the BC7 tier's region manifest
    std::vector<std::pair<LLUUID, LLUUID> > mGroups;   // <SS:Nexii/> FullID -> GroupID from Stage C. A null group is a real answer meaning "set to no group", and keeping it is what stops the object being asked about again on every future visit.

    size_t promotedCount() const;
    size_t approxBytes() const;
};

// <SS:Nexii> Reduce a whole region file to the part of it a format bump cannot invalidate: FullID-keyed presence history and terrain. Everything a bump has ever been about - the stored local id, the CRC it was last named with, the blob, the live update flags, the score, the promotion flag and both epoch bits - is cleared, along with the file's own CacheID, so what survives cannot paint an object, cannot answer a simulator probe and cannot claim an epoch. Exposed rather than kept private to deserializeSalvage so the stripping rule itself is testable, which matters more than the parse.
void ssROCStripToTenure(SSROCRegionFile& file);

// Build the arrival paint order for a loaded region file. Returns indices into `file.mRecords`, most permanent and largest first, because painting a whole region takes many frames and what lands in the first of them decides whether the place reads as itself immediately or as a scatter of props with the buildings missing.
//
// Records with no blob, no last-known local id, a hard disqualifier or a STALE local-id epoch are never planned - none of the first three can be rezzed back, and the fourth would be rezzed at an id the simulator has since reissued to somebody else.
//
// Linkset children whose root did not survive the plan are dropped rather than injected as orphans: a child's cached position is parent-relative, so on its own it is a fragment with nowhere to stand.
void ssROCBuildInjectPlan(const SSROCRegionFile& file, const SSROCInjectPolicy& policy, std::vector<U32>& out);

// ---------------------------------------------------------------------------
// Backing the stock protocol object cache
// ---------------------------------------------------------------------------
//
// The ROC already stores everything an .slc entry holds and more - the byte-identical data-packer blob, the CRC the simulator last named, the local id, the update flags the .slc disk format does not even have - so keeping a second copy of the same objects on disk buys nothing. When SSROCBackObjectCache is on, LLViewerRegion::loadObjectCache fills its entry map from these records instead of from the .slc, and the protocol code above it is untouched: probeCache and cacheFullUpdate read a map and neither knows nor cares where it came from.
//
// SEEDING IS NOT PAINTING, and the distinction is the whole safety argument. A seeded entry is created INVALID, exactly as one read from the .slc is, so it is inert until the simulator's own probe validates it. It therefore carries no promotion gate, no recency window and no disqualifier test - a physical object belongs in the protocol cache precisely as much as it belongs in the .slc, and withholding it would only turn a probe hit into a request the simulator did not have to answer.

// Why a record may not be handed to the protocol object cache. A CLOSED set, counted per region and reported by name, because a bare refusal on this path is indistinguishable from a cache that was never warm - and the difference is visible to the simulator as request traffic.
enum ESSROCSeedRefusal : U8
{
    SSROC_SEED_OK           = 0,
    SSROC_SEED_NO_BLOB      = 1,  // no stored blob, or one larger than the stock entry body limit - it could not be an .slc entry either
    SSROC_SEED_NO_LOCAL_ID  = 2,  // never carried a local id, so there is no key to file it under
    SSROC_SEED_STALE_EPOCH  = 3,  // the CacheID moved on while this record went unmentioned: its stored id names whatever the simulator has since reissued it to
    SSROC_SEED_RESERVED_ID  = 4,  // an id at or above SSROC_SYNTH_ID_FLOOR must never enter the protocol cache, because the stock cache-miss guard drops that range and the object would then never be requested
    SSROC_SEED_DUPLICATE_ID = 5,  // a fresher record already claimed this local id this visit
    SSROC_SEED_COUNT        = 6,
};

// Why a region visit did or did not have its protocol object cache filled from the ROC. A CLOSED set, latched at every exit and reported once per region, following the rule this project already learned the hard way on the ghost path: no reason may ever be inferred from a default value, and a decline must never be indistinguishable from never having run.
//
// Every outcome other than SEEDED means the STOCK .slc path runs for that region, untouched. That is deliberate: a decline costs the ROC its win for one visit and costs the simulator nothing at all.
enum ESSROCBackOutcome : U8
{
    SSROC_BACK_DISABLED        = 0,  // the setting is off, or the store is not running
    SSROC_BACK_NOT_TRACKED     = 1,  // the ledger never saw this region added - what enabling the cache mid-session looks like
    SSROC_BACK_NO_HANDSHAKE    = 2,  // no CacheID yet, so the local-id epoch is unknowable and no stored id may be trusted
    SSROC_BACK_LOAD_UNRESOLVED = 3,  // the .roc read had not landed and the blocking fallback did not produce one either
    SSROC_BACK_EPOCH_UNDECIDED = 4,  // both facts arrived but the epoch pass has not run - structurally unreachable, named so it can never be silent
    SSROC_BACK_EPOCH_UNKNOWN   = 5,  // the file carries no CacheID at all, because the visit that wrote it never saw a handshake - not a restart, and materially different from one
    SSROC_BACK_CACHEID_CHANGED = 6,  // the simulator restarted: every stored local id belongs to somebody else now, exactly as the stock code decides when it discards the whole .slc
    SSROC_BACK_SANDBOX         = 7,  // sandbox regions are never recorded, so there is nothing to fill from
    SSROC_BACK_NO_RECORDS      = 8,  // first visit, or a region whose ledger is empty
    SSROC_BACK_PLAN_EMPTY      = 9,  // records exist but none of them could be handed over - the per-record refusal histogram says which
    SSROC_BACK_SEEDED          = 10, // the map was filled from the ROC and the .slc was not read
    SSROC_BACK_COUNT           = 11,
};

const char* ssROCBackOutcomeName(U8 outcome);

const char* ssROCSeedRefusalName(U8 refusal);

struct SSROCSeedStats
{
    SSROCSeedStats() { for (U32 i = 0; i < SSROC_SEED_COUNT; ++i) mCounts[i] = 0; }
    U32 mCounts[SSROC_SEED_COUNT];
    std::string describe() const;
};

// Which records may seed the protocol object cache, in the order they should be offered. Pure over the record set so it can be tested offline against the same store the viewer uses.
//
// `same_epoch` is the file-level CacheID comparison, kept as a separate argument rather than inferred from the per-record bits: on a mismatch the stock code discards the WHOLE .slc, so refusing every record here is what makes the simulator-facing answer identical rather than merely similar.
//
// Ordered by last-confirmed descending so that when two records claim one local id - which the epoch bits are meant to prevent and which must still be decided rather than left to map iteration order - the more recently confirmed one wins.
void ssROCBuildSeedPlan(const std::vector<SSROCRecord>& records, bool same_epoch, U32 max_seed, std::vector<U32>& out, SSROCSeedStats& stats);

using SSROCFilePtr      = std::shared_ptr<SSROCRegionFile>;
using SSROCLoadCallback = std::function<void(U64 handle, SSROCFilePtr file)>;

// The store owns disk layout, async IO and the disk budget. It deliberately knows nothing about ghosts, scoring or reconciliation - those live above it, so the container can be tested and soaked on its own.
class SSROCStore : public LLSingleton<SSROCStore>
{
    LLSINGLETON(SSROCStore);
    ~SSROCStore();

public:
    // SSROCEnabled gates every entry point. While it is off the store does nothing at all: no directory is created, no file is read or written, and no worker is posted.
    static bool enabled();

    void initStore();

    // Initialise on demand. initStore() runs once at startup and does nothing while the feature is off, so without this the store stays dead for the whole session when the user enables ROC at the login screen or in preferences - which is exactly how anyone would try it for the first time.
    void ensureInitialized();
    // Call BEFORE the world is torn down at quit. From here on saves are written inline on the calling thread rather than posted, because the region removals that produce the final captures happen while the worker pool is already winding down - posting them means they are never executed and the last visit to every region is silently lost.
    void beginShutdown();

    void shutdownStore();   // drains anything still in flight from before beginShutdown
    bool isInitialized() const { return mInitialized; }

    // Async load. The callback always fires on the main thread, exactly once, with a null file when the region has no cache or the file was unreadable.
    void loadRegionAsync(U64 handle, SSROCLoadCallback cb);

    // Blocking load on the calling thread, for the one caller that cannot be deferred: LLViewerRegion::loadObjectCache runs inside unpackRegionHandshake and the map it fills decides bit 1 of the RegionHandshakeReply - the bit that tells the simulator whether to send cache probes at all. Answering that bit from a half-arrived cache would change what the simulator sends, which is the one thing this integration may not do. This is NOT a new stall: it stands where the stock synchronous .slc read stands, replaces it, and only runs at all when the async read has not already landed.
    SSROCFilePtr loadRegionBlocking(U64 handle);

    // Async save of a completed snapshot. The caller must not touch the file afterwards.
    void saveRegionAsync(SSROCFilePtr file);

    void deleteRegion(U64 handle);
    void purgeAll();        // hooked into every cache-clear path - a region cache is a record of where you have been and must die with the rest of the cache

    std::string storeDir() const { return mStoreDir; }
    std::string regionFilePath(U64 handle) const;

    U64  diskBytesUsed() const { return mDiskBytesUsed; }
    void refreshDiskUsage();
    void enforceDiskBudget();   // lax watermark: nothing is evicted until the budget is approached, then whole least-recently-visited region files are dropped

    struct Metrics
    {
        std::atomic<U32> mLoadsOk{0};
        std::atomic<U32> mLoadsMissing{0};
        std::atomic<U32> mLoadsCorrupt{0};
        // <SS:Nexii> A file written by an older format version is NOT corruption and must never be counted as it. Every format bump orphans every existing file at once, so folding the two together means a routine upgrade reports the user's whole cache as damaged - which is exactly what happened after 7 to 8, and it is alarming, wrong, and completely uninformative about the thing it names.
        std::atomic<U32> mLoadsOldVersion{0};
        // <SS:Nexii> Counted apart from both of the above, because it is the good outcome: an older file whose presence history was carried forward while everything the bump was about was discarded. A bump that reports 48 salvaged files has cost the owner one cold visit per region; one that reports 48 orphaned files has cost every record on the grid three more days before it can promote.
        std::atomic<U32> mLoadsSalvaged{0};
        std::atomic<U32> mSavesOk{0};
        std::atomic<U32> mSavesFailed{0};
        std::atomic<U32> mFilesEvicted{0};
        std::atomic<U64> mBytesRead{0};
        std::atomic<U64> mBytesWritten{0};
    };
    Metrics& metrics() { return mMetrics; }
    std::string metricsString() const;

    // Serialisation is exposed for unit testing and for the debug commands - both are pure functions over a buffer with no store state involved.
    static bool serialize(const SSROCRegionFile& file, std::vector<U8>& out);
    static bool deserialize(const U8* data, size_t size, SSROCRegionFile& out);

    // <SS:Nexii> True when the buffer is a well formed ROC file from a DIFFERENT format version - readable enough to identify, deliberately not readable as data. Separate from deserialize so the loader can tell "this is last week's format" from "these bytes are damaged" without either of them guessing.
    static bool isOtherVersion(const U8* data, size_t size);

    // <SS:Nexii> Read a file from an older but layout-compatible version for its TENURE ONLY. Succeeds for versions SSROC_MIN_SALVAGE_VERSION..SSROC_FORMAT_VERSION and strips every field either bump was about, so what comes back can be neither painted nor offered to the protocol cache - it is presence history and terrain, nothing that could stand an object anywhere. A file from a NEWER version is refused outright: this reader has no way to know what its records mean.
    static bool deserializeSalvage(const U8* data, size_t size, SSROCRegionFile& out);

    // Internal, public only so the worker-side scope guard can reach it: retires one in-flight IO op and wakes a shutdown that is waiting to drain.
    void endOp();

private:
    // <SS:Nexii> The single parser behind both deserialize and deserializeSalvage, so one layout is never read by two subtly different readers.
    static bool deserializeVersioned(const U8* data, size_t size, SSROCRegionFile& out, U32 accept_version);

    SSROCFilePtr loadRegionBody(U64 handle, const std::string& path);
    std::string gridKey() const;
    bool        writeFileBlocking(const std::string& path, const std::vector<U8>& bytes);
    static bool readFileBlocking(const std::string& path, std::vector<U8>& bytes);

    bool                    mInitialized;
    bool                    mShuttingDown;
    bool                    mReadOnly;      // a second viewer instance shares this cache directory and must never write to it, mirroring how LLVOCache refuses in read-only mode
    std::string             mStoreDir;
    std::atomic<U64>        mDiskBytesUsed{0};
    std::atomic<S32>        mPendingOps{0};
    mutable std::mutex      mDrainMutex;
    std::condition_variable mDrainCV;
    Metrics                 mMetrics;
    boost::signals2::scoped_connection mBudgetSignalConnection;
};

#endif // SS_ROCCACHE_H
