/**
 * @file ssbc7manifest.h
 * @brief Squeeze region texture manifests - remembers which texture uuids mattered in a region and, on returning, puts their BC7 in video memory off the local disk before anything asks, see doc/viewer/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_BC7MANIFEST_H
#define SS_BC7MANIFEST_H

#include "lluuid.h"

#include <string>
#include <unordered_map>
#include <vector>

// THE GAP THIS CLOSES. The BC7 store only ever helps once something ASKS for a uuid: the interest list has to reach the object, the fetcher has to want the texture and the pipeline has to decide it is wanted, and only then is the store consulted. On arriving somewhere you have been before, the bytes are already on the local disk in the exact form the GPU wants them, and the viewer sits waiting for the network to tell it something it already knows.
//
// A MANIFEST IS A LIST OF UUIDS, NEVER A COPY OF THE BYTES, and that is the whole design (doc/viewer/super_compressed_textures.md, "Q1: why manifests beat physical region/agent data files"). The same bodies, kits and building textures recur across regions, so a physical per-region file would multiply the store by the number of places a texture is seen, for nothing a list of uuids does not already deliver. Sixteen bytes per uuid buys the teleport warm-up, the semantic eviction pin and shared-asset correctness all at once.
//
// SCOPE. Region manifests only. Agent manifests are the same mechanism keyed by avatar uuid and hooked at processAvatarAppearance instead of region change, and are deliberately NOT built here - the read path, the cap policy and the file format below are all key-agnostic, so adding them later is a second filename prefix and a second recorder rather than a second design.

// ---------------------------------------------------------------------------
// Where it lives, and the shape of the file
// ---------------------------------------------------------------------------

// INSIDE the BC7 store's own directory rather than in the region object cache's .roc file, and the reason is ownership rather than convenience. ssroccache.h does define a manifest section id, and folding this in there is a sensible consolidation ONCE ROC HAS SETTLED - but a texture manifest is a Squeeze concern that merely happens to be keyed by region, and putting it in ROC's file would make every ROC format change a Squeeze format change. Living under bc7cache also buys the privacy requirement for free: SSBC7Store::purgeAll is a recursive deleteDirAndContents of the store directory, so "clear cache" takes the record of where you went with it, which is the fourth of the four critical fixes and not something to re-litigate per subdirectory.
constexpr const char* SSBC7_MANIFEST_DIR_NAME = "manifests";
constexpr const char* SSBC7_MANIFEST_EXT      = ".uml";

constexpr U32 SSBC7_MANIFEST_MAGIC        = 0x4D4C4D53;  // 'SMLM' little-endian - a wrong-file check that costs four bytes and one comparison
constexpr U32 SSBC7_MANIFEST_VERSION      = 1;
constexpr U32 SSBC7_MANIFEST_HEADER_SIZE  = 64;          // fixed, so a reader can always decide whether it has a whole header before it trusts one field of it
constexpr U32 SSBC7_MANIFEST_ENTRY_SIZE   = 20;          // uuid plus the weight that decides both warm order and what the cap sacrifices

// PER-REGION CAP, and what it drops is stated rather than implied. 4096 uuids is 80 KB of file and comfortably more than a dense region's servable texture set; 128 manifests is about 10 MB of disk for the whole grid's worth of places one person visits.
constexpr U32 SSBC7_MANIFEST_MAX_ENTRIES  = 4096;
constexpr U32 SSBC7_MANIFEST_MAX_FILES    = 128;

// The engine's own pacing. Deliberately slow: nothing here is latency critical to the millisecond, and the one thing that would be unforgivable is competing with the textures the user is actually looking at.
constexpr F64 SSBC7_MANIFEST_TICK_SECONDS    = 0.25;  // how often a warm pass may run - fast enough that a few hundred textures are in place inside the first few seconds, which is the whole window this feature has to win in
constexpr U32 SSBC7_MANIFEST_RECORD_SECONDS  = 15;    // how often the recorder sweeps the texture list; one walk of gTextureList and a hash insert per entry
constexpr U32 SSBC7_MANIFEST_WARM_PER_TICK   = 32;    // pre-warm reads issued by one pass. Well inside the shared 256 deep reader queue, so this path can never fill it and leave a foreground read with nowhere to go, and the main-thread cost of a pass stays under a millisecond
constexpr U32 SSBC7_MANIFEST_WARM_MAX        = 2048;  // pre-warms attempted per region arrival, a bound on the whole episode rather than on its rate
constexpr U32 SSBC7_MANIFEST_VERIFY_PER_TICK = 32;    // issued pre-warms examined per pass to see whether they actually landed in video memory

// ---------------------------------------------------------------------------
// Verdicts
// ---------------------------------------------------------------------------

// Every refusal is named and counted, because "manifests are off", "the manifest was refused as corrupt" and "the manifest loaded and every uuid in it was already resident" are three completely different situations that a bare return false renders identical. This project has paid for that four times, most recently as six unnamed exits in readBlobPrefix.
enum ESSBC7ManifestVerdict
{
    SSBC7_MANIFEST_OK = 0,
    SSBC7_MANIFEST_DECLINE_NO_FILE,      // no manifest for this region yet, which is the ordinary answer on a first visit
    SSBC7_MANIFEST_DECLINE_UNREADABLE,   // the file is there and the read failed - a permissions or media problem, not a format one
    SSBC7_MANIFEST_DECLINE_SHORT,        // fewer bytes on disk than one header, so there is nothing to check the rest against
    SSBC7_MANIFEST_DECLINE_MAGIC,        // not one of ours at all
    SSBC7_MANIFEST_DECLINE_VERSION,      // written by a different build; manifests are derived data and a version bump orphans them on purpose
    SSBC7_MANIFEST_DECLINE_HEADER_CRC,   // the header did not checksum, so not one field in it may be believed - including the count
    SSBC7_MANIFEST_DECLINE_COUNT,        // the count is past the cap; a corrupt length must never be allowed to become an allocation
    SSBC7_MANIFEST_DECLINE_TRUNCATED,    // the file size and the header's count disagree - the torn-write case, refused rather than read short
    SSBC7_MANIFEST_DECLINE_PAYLOAD_CRC,  // the uuid list itself did not checksum
    SSBC7_MANIFEST_DECLINE_HANDLE,       // the header names a different region than the filename does, which means a stale or shuffled file
    SSBC7_MANIFEST_DECLINE_WRITE,        // the save failed; the previous manifest is left exactly as it was, because a half-written one is worse than an old one
    SSBC7_MANIFEST_VERDICT_COUNT
};

const char* ssBC7ManifestVerdictName(ESSBC7ManifestVerdict verdict);

// ---------------------------------------------------------------------------
// Pure file format and cap policy, in ssbc7manifestfile.cpp - no viewer globals, no threads, no settings, so the offline harness exercises exactly the code that ships
// ---------------------------------------------------------------------------

// The WEIGHT is the largest on-screen area, in texels, this region ever wanted the texture at. It does two jobs at once and that is why it is stored rather than recomputed: it orders the pre-warm so the biggest contributors to video memory land first, and it is what the cap sacrifices from the bottom of.
struct SSBC7ManifestEntry
{
    LLUUID mUUID;
    U32    mWeight = 0;
};

struct SSBC7ManifestHeader
{
    U32 mMagic       = SSBC7_MANIFEST_MAGIC;
    U32 mVersion     = SSBC7_MANIFEST_VERSION;
    U64 mRegionHandle = 0;   // checked against the filename, so a file copied or renamed between regions is refused instead of warming the wrong place
    U64 mLastSeen    = 0;    // wall clock seconds - the LRU key for the directory cap, and the pin a later semantic eviction would read
    U32 mCount       = 0;
    U32 mDropped     = 0;    // entries the cap has forced out over the LIFE of this manifest. Non-zero means the list is a FLOOR and not a complete account, which is exactly the thing silent truncation hides
    U32 mVisits      = 0;
    U32 mPayloadCRC  = 0;
};

// Byte-exact serialisation, exposed so the round trip can be tested without a filesystem.
void ssBC7ManifestSerialise(const SSBC7ManifestHeader& hdr, const std::vector<SSBC7ManifestEntry>& entries, std::vector<U8>& out);

// Parses and VALIDATES. Never returns a partially filled result: on any refusal the outputs are left empty, because a caller that got half a manifest would warm half a region and log that it had warmed one.
ESSBC7ManifestVerdict ssBC7ManifestParse(const U8* data, size_t size, SSBC7ManifestHeader& out_hdr, std::vector<SSBC7ManifestEntry>& out_entries);

// Reads one manifest off disk. expect_handle is REQUIRED for the same reason ssBC7VerifyBlob requires a uuid: the filename is the only thing tying a file to a region, and a filename is exactly the part of a cache that survives being copied about.
ESSBC7ManifestVerdict ssBC7ManifestRead(const std::string& path, U64 expect_handle, SSBC7ManifestHeader& out_hdr, std::vector<SSBC7ManifestEntry>& out_entries);

// Writes via a temporary plus a rename, so the torn file this reader is built to refuse is one this writer cannot produce in the first place. Refusing corrupt input is the safety net, not the plan.
ESSBC7ManifestVerdict ssBC7ManifestWrite(const std::string& path, const SSBC7ManifestHeader& hdr, const std::vector<SSBC7ManifestEntry>& entries);

// Grid scoped because SL and OpenSim region coordinates collide outright - the same handle names different places on different grids, and warming one from the other's manifest would be silently wrong rather than merely useless.
std::string ssBC7ManifestFileName(const std::string& grid_id, U64 region_handle);

// Directory LRU by the manifests' own mLastSeen, generalising the VOCache mTime LRU. Returns how many files it removed; a file it cannot parse is removed too, because an unreadable manifest is dead weight that would otherwise sit in the cap forever.
U32 ssBC7ManifestPruneDir(const std::string& dir, U32 keep);

// A bounded per-region uuid set that says what it gave up.
//
// WHAT THE CAP DROPS, and why it is not the want list's FIFO. The want list is a work queue, so its oldest entry is its least urgent and dropping from the front is right. A manifest is not a queue - it is a description of a place, read back once, in full, on the next visit - so age says nothing useful about an entry and area says everything: the textures that dominate video memory in a region are the ones that covered the most screen there. So the cap sacrifices the LIGHTEST entries, the ones only ever glimpsed at a few pixels across, which are simultaneously the cheapest to forget and the least missed. Every drop is counted and the running total is written into the manifest header, so a readout can say "at least this many" rather than implying it remembered everything.
class SSBC7ManifestSet
{
public:
    explicit SSBC7ManifestSet(U32 cap);

    void clear();

    // Keeps the LARGEST weight ever seen for a uuid, because a texture that was once close is a texture this region can put close again.
    void note(const LLUUID& id, U32 weight);

    // Folds a manifest read back off disk into what this visit has seen. Weights are HALVED on the way in so a set built over many visits keeps re-earning its place: a texture that still matters is noted again at full weight this visit, one that has been removed from the region fades below the newcomers over a few visits and is dropped by the cap rather than pinning it forever.
    void mergeDecayed(const std::vector<SSBC7ManifestEntry>& in);

    // Heaviest first, which is also the order the pre-warm wants.
    void take(std::vector<SSBC7ManifestEntry>& out) const;

    size_t size() const     { return mWeights.size(); }
    U32    capacity() const { return mCap; }
    U32    dropped() const  { return mDropped; }
    const LLUUID& lastDropped() const { return mLastDropped; }

private:
    void pruneToCap();

    std::unordered_map<LLUUID, U32> mWeights;
    U32                             mCap;
    U32                             mDropped;
    LLUUID                          mLastDropped;
};

// ---------------------------------------------------------------------------
// The engine, in ssbc7manifest.cpp
// ---------------------------------------------------------------------------

// Main thread. Resolves the store and caches the policy. Starts no threads - the load and the save go on the existing BC7Encode pool and the pre-warm reads go through the existing reader pool, because a third pool for work nobody is waiting on would only be more shutdown ordering to get wrong.
void ssBC7ManifestInit();

// Main thread. Re-reads the SSSqueezeManifest settings so the feature can be switched on without a restart, exactly as the encode, read and promotion gates already can.
void ssBC7ManifestRefreshPolicy();

// Main thread, every frame, and on almost all of them one pointer comparison. Called from the head of ssBC7PromoteTick rather than from a new hook in llviewerregion.cpp: the promotion engine already runs a main-thread tick and gAgent already knows which region the agent is in, so detecting the change here costs nothing and adds no stock surface at all.
void ssBC7ManifestTick();

// Main thread. Writes the current region's manifest out before the viewer goes away, so a session that ended somewhere new is not a session that learned nothing.
void ssBC7ManifestBeginShutdown();
void ssBC7ManifestShutdown();

// The numbers worth putting in front of a person, as numbers rather than inside a log line. Wired into the overlay separately by the owner - nothing in this module edits ssstatsview.cpp.
struct SSBC7ManifestStats
{
    U32 mManifestsLoaded    = 0;   // region manifests read and accepted this session
    U32 mManifestsRefused   = 0;   // read and refused - corrupt, torn, wrong version or wrong region
    U32 mManifestsSaved     = 0;
    U32 mEntriesLoaded      = 0;   // uuids the accepted manifests named
    U32 mWarmIssued         = 0;   // pre-warm reads actually posted
    U32 mWarmLanded         = 0;   // of those, the ones observed resident in video memory afterwards - the only one of these numbers that is the feature working
    U32 mWarmAlreadyResident= 0;   // named by a manifest and already in video memory, so the warm cost nothing and saved nothing
    U32 mWarmNoRecord       = 0;   // named by a manifest with no BC7 record on this disk; a want-list candidate at most, NEVER a network fetch from this path
    U32 mWarmDeclined       = 0;   // the read path refused the texture, which is the ordinary answer for sculpts, bakes and alpha
    U32 mWarmRefused        = 0;   // the reader queue was busy; retried on a later pass and never a block
    U64 mWarmBytesInPlace   = 0;   // BC7 bytes the pre-warm had in video memory, attributed only to entries observed resident
    U64 mWarmBytesSaved     = 0;   // what those would have cost uncompressed, minus the above
    U32 mRecorded           = 0;   // uuids the current region's set holds right now
    U32 mRecordDropped      = 0;   // uuids the per-region cap has sacrificed this session; non-zero means every manifest written since is a floor
    U32 mLastVerdict        = 0;   // ESSBC7ManifestVerdict - how the last load ended
};
SSBC7ManifestStats ssBC7ManifestStatsNow();

std::string ssBC7ManifestMetricsString();

#endif // SS_BC7MANIFEST_H
