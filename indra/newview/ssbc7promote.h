/**
 * @file ssbc7promote.h
 * @brief Squeeze promotion engine - drains the BC7 want list, first from J2C already complete on this disk and then, under a hard throttle, by completing partial J2C over the network, see doc/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_BC7PROMOTE_H
#define SS_BC7PROMOTE_H

#include "lluuid.h"

#include <list>
#include <string>
#include <unordered_map>
#include <vector>

// Under FULL-RES-ONLY the demand path encodes only the free case - a texture that happened to decode at discard 0 with the whole asset in hand - so almost every BC7 record in the store has to be created here. This is the PRIMARY fill mechanism, not an optional extra, which is why it runs on a tick of its own rather than hanging off the once-a-minute eviction pass.
//
// TWO TIERS, IN STRICT ORDER, and the order is the entire safety argument. Tier (a) is free: a texture whose J2C is already complete on this disk needs no network at all, so it is drained first, always, and until it is empty tier (b) is not even considered. Tier (b) spends the user's bandwidth on textures they are not looking at, so it is throttled by a token bucket, capped for the whole session, refused while the texture fetcher is doing anything for the user, and refused outright during teleport and login.

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr U32 SSBC7_PROMOTE_TICK_SECONDS      = 2;     // how often a pass may run; the frame cost on every other tick is one clock comparison
constexpr U32 SSBC7_PROMOTE_BURST_SECONDS     = 4;     // how much unspent credit the token bucket may carry, so a genuinely idle minute does not turn into a minute-long burst the moment something becomes eligible
constexpr size_t SSBC7_PROMOTE_SCAN_BATCH     = 48;    // want-list entries examined by one scan; each one costs a header-entry read, and the batch is what keeps that off any single long-held lock
// <SS:Nexii> Squeeze capacity-driven promotion - there is deliberately NO fixed number of fused encodes per pass any more. A pass posts as many as there are SPARE WORKERS: pool width, minus workers inside an encode right now, minus fused encodes this engine has queued that no worker has picked up yet. That number is measured on every pass, so a machine with cores to spare fills them and a machine under load posts nothing, and it is gated on the adaptive controller reporting that the pool is keeping up, so the moment throughput falls behind - because of other load on the box as much as because of texture volume - the engine stops adding until it recovers. The old ceiling of four per two seconds was sized for an encoder twenty times slower than the one linked now and left sixteen cores idle behind a list of two hundred textures.
//
// The follow-up interval is how soon a pass may run again after one that found work to do or workers still busy: fast enough that a drained pool is refilled within a frame or two of a worker finishing, slow enough that the main thread is not probing an atomic and a want list every frame. The idle interval above still applies once nothing is waiting.
constexpr F64 SSBC7_PROMOTE_FOLLOWUP_SECONDS  = 0.25;
// </SS:Nexii>
constexpr size_t SSBC7_PROMOTE_NET_CANDIDATES = 256;   // partial uuids the scan is allowed to hand forward for network completion; a bound here is what stops one walk through a mall from becoming a fetch list nothing will ever get through
// <SS:Nexii> In-flight fetches are the ONLY bound on network promotion, and the number is chosen to match what the machine can actually consume rather than to be unobtrusive. Two was set to be "never noticeable" and succeeded: at a fetch latency of a few hundred milliseconds it kept a four-thread encode pool idle almost all of the time, which is the opposite of the point - the engine exists to keep that pool busy.
//
// Derived from the encode pool because that is the real consumer. Each worker spends roughly a sixth of a second on a 1024 square texture and a fetch takes several times that, so several fetches must be outstanding per worker just to break even. Four per worker is the ratio, floored so a single-worker machine still pipelines and ceilinged well inside the AP_TEXTURE policy class's own connection and pipelining limits, which are what actually pace the transfers.
S32 ssBC7PromoteMaxInFlight();
constexpr U32 SSBC7_PROMOTE_NET_TIMEOUT_SECS  = 120;   // a continuation fetch that has not reached DONE by now is abandoned and its worker deleted, so a stalled request cannot hold a slot for the session
constexpr S32 SSBC7_PROMOTE_FETCH_IDLE_MAX    = 4;     // texture-fetch requests still outstanding for the USER at which the fetcher stops counting as idle. llviewertexturelist.cpp:924 uses zero for the same question; a small allowance is what stops a single lingering DONE worker from switching the engine off for the whole session
constexpr size_t SSBC7_PROMOTE_WANT_CAP       = 8192;  // want-list ceiling, about 128 KB of uuids

// ---------------------------------------------------------------------------
// Verdicts
// ---------------------------------------------------------------------------

// Why a pass did what it did. Every exit is named, counted and folded into the metrics line, because "promotion is off" and "promotion ran and declined everything" produce identical silence otherwise - the failure this project has already paid for three times, most recently as six unnamed exits in readBlobPrefix that made "no record" and "the index is corrupt" indistinguishable from a log file.
//
// There is deliberately NO verdict for "the tick interval has not elapsed". That exit happens on almost every frame and changes nothing, so counting it would put an eight figure number beside eighteen meaningful ones and make the tally unreadable - which is the same failure in the other direction.
enum ESSBC7PromoteVerdict
{
    SSBC7_PROMOTE_RAN_LOCAL = 0,        // tier (a) posted at least one fused local encode this pass
    SSBC7_PROMOTE_RAN_SCAN,             // tier (a) posted a want-list scan; the encodes it finds are posted by the scan itself
    SSBC7_PROMOTE_RAN_NETWORK,          // tier (b) issued at least one continuation fetch
    SSBC7_PROMOTE_IDLE_EMPTY,           // nothing is waiting, which is the answer that means the engine has caught up
    SSBC7_PROMOTE_DECLINE_OFF,          // SSSqueezeEnabled, SSSqueezeBackgroundEncode or SSSqueezePromote is off, or the GPU has no BPTC so a record would have nothing to serve to
    SSBC7_PROMOTE_DECLINE_NOT_READY,    // the store never came up, is a read-only second instance, or shutdown has begun
    SSBC7_PROMOTE_DECLINE_POOL_BUSY,    // the encode pool still has demand-path work in flight, and demand always outranks backfill
    SSBC7_PROMOTE_DECLINE_SCAN_RUNNING, // a scan is already out on the pool; a second one would chase the same uuids
    SSBC7_PROMOTE_DECLINE_POST_FAILED,  // tryPost refused, which means a full queue and never an error
    SSBC7_PROMOTE_DECLINE_BACKPRESSURE, // the pinned-bytes budget is exhausted, so the decoded raws already queued are as much memory as this is allowed to hold
    SSBC7_PROMOTE_DECLINE_STORE_FULL,   // the store is already over the user's SSBC7CacheSize, so anything added now only makes eviction drop something else
    SSBC7_PROMOTE_DECLINE_NET_OFF,      // tier (a) is exhausted and SSSqueezeNetworkPromote is off - the one decline the checkbox is supposed to produce
    SSBC7_PROMOTE_DECLINE_NET_TELEPORT, // a teleport is in progress; the fetch pipeline is about to be wiped and the bandwidth belongs to arriving
    SSBC7_PROMOTE_DECLINE_NET_LOGIN,    // not yet at STATE_STARTED, or disconnected
    SSBC7_PROMOTE_DECLINE_NET_BUSY,     // the texture fetcher has foreground work, and foreground fetching always wins
    SSBC7_PROMOTE_DECLINE_NET_BANDWIDTH,// the token bucket is empty this second
    SSBC7_PROMOTE_DECLINE_NET_SESSION,  // the whole-session byte ceiling has been reached and nothing more will be fetched until the viewer restarts
    SSBC7_PROMOTE_DECLINE_NET_IN_FLIGHT,// the continuation-fetch concurrency cap is already met
    SSBC7_PROMOTE_DECLINE_NET_NO_TARGET,// nothing on the network candidate list survived targeting: no live texture, no known size, or already being fetched for the user
    // <SS:Nexii/> Squeeze adaptive quality - appended rather than inserted, because the ordinals are what index s_promote_verdict_names and reordering them would silently mislabel every counter. This is the third and last tier: with nothing left to create, the engine goes back over records that were encoded at a lower profile and re-encodes them at the best one.
    SSBC7_PROMOTE_RAN_UPGRADE,
    // <SS:Nexii/> Squeeze capacity-driven promotion - appended for the same reason. Two ways tier (a) now declines: every worker is busy or already has queued work, or the adaptive controller says the pool is not keeping up and adding to it would only deepen the hole.
    SSBC7_PROMOTE_DECLINE_NO_SPARE_WORKER,
    SSBC7_PROMOTE_DECLINE_NOT_KEEPING_UP,
    SSBC7_PROMOTE_VERDICT_COUNT
};

const char* ssBC7PromoteVerdictName(ESSBC7PromoteVerdict verdict);

// ---------------------------------------------------------------------------
// Pure policy, in ssbc7promotepolicy.cpp - no viewer globals, no threads, no IO, so the offline harness exercises exactly the code that ships
// ---------------------------------------------------------------------------

// The J2C tier's completeness rule, written down in one place because it is not obvious and exists nowhere else in the tree as a statement: LLTextureCacheWorker records the ASSET's total size in the entry, but a fetch that only had part of the asset writes total+1 instead as an in-band partial sentinel (lltexturefetch.cpp:2079-2088). "Have" is the fixed header record plus the body file, so that deliberate off-by-one is what makes a partial always fall exactly one byte short of complete.
enum ESSBC7J2CExtent
{
    SSBC7_J2C_COMPLETE = 0,     // the whole asset is on this disk and can be encoded with no network at all
    SSBC7_J2C_PARTIAL,          // some bytes, not all - this is precisely the set tier (b) exists for
    SSBC7_J2C_UNKNOWN           // the entry never learned the asset's total size, so completeness cannot be decided in either direction and guessing would be worse than declining
};

ESSBC7J2CExtent ssBC7J2CExtent(S32 image_size, S32 body_size, S32 entry_size, S32& out_have, S32& out_total);

// A bounded want list that says what it gave up. NOT thread safe on its own - the encode queue owns one behind the mutex it already holds for its in-flight set, which is the same mutex the verdict tally is sized under.
//
// FIFO AT THE BOTTOM, LIFO AT THE TOP, and the asymmetry is deliberate. Work is taken NEWEST first because the most recently seen texture is the one the user is most likely still stood in front of, and entries are dropped OLDEST first because a walk through a busy mall must not be able to push out the room the user is actually in. The alternative - refusing new entries once full, which is what this replaced - silently pins the list to whatever the first eight thousand textures of the session happened to be and reads from a log line as though the engine had everything in hand.
class SSBC7WantList
{
public:
    explicit SSBC7WantList(size_t cap);

    // True when the uuid was newly added. When the list was at its cap the OLDEST entry is evicted to make room, copied into out_dropped and counted, so the caller can name what it gave up rather than truncating in silence.
    bool add(const LLUUID& id, LLUUID& out_dropped, bool& out_did_drop);

    bool erase(const LLUUID& id);
    bool contains(const LLUUID& id) const;

    // Newest first, and REMOVES what it hands back so two passes can never chase the same texture. A caller that cannot use one puts it back with add().
    size_t take(size_t max_count, std::vector<LLUUID>& out);

    void snapshot(std::vector<LLUUID>& out) const;

    size_t size() const { return mIndex.size(); }
    size_t capacity() const { return mCap; }
    U32    droppedTotal() const { return mDropped; }

private:
    typedef std::list<LLUUID> order_t;

    order_t                                       mOrder;   // oldest at the front, newest at the back
    std::unordered_map<LLUUID, order_t::iterator> mIndex;
    size_t                                        mCap;
    U32                                           mDropped;
};

// A token bucket over bytes plus a hard whole-session ceiling. Both are needed and they answer different questions: the bucket says "never faster than this", the ceiling says "never more than this, at all". A rate limit on its own still spends a gigabyte given a long enough session, which is exactly what somebody on a metered connection is afraid of, and a ceiling on its own would spend it in the first ninety seconds.
//
// NOT thread safe; the engine owns one and only touches it from the main thread.
class SSBC7NetBudget
{
public:
    SSBC7NetBudget();

    void configure(U64 bytes_per_second, U64 session_cap_bytes);

    // Accrues tokens for the elapsed wall time. The first call only sets the clock, so a viewer that sat at the login screen for an hour does not arrive in-world holding an hour of credit.
    void advance(F64 now_seconds);

    bool canSpend(U64 bytes) const;
    bool spend(U64 bytes);      // false and completely unchanged when it would break either limit
    void refund(U64 bytes);     // a request that was never issued hands its estimate straight back

    U64  spentTotal() const { return mSpent; }
    U64  available() const  { return mTokens; }
    U64  sessionCap() const { return mSessionCap; }
    bool sessionCapReached() const { return mSessionCap != 0 && mSpent >= mSessionCap; }

private:
    U64  mRate;         // bytes per second; zero means the throttle refuses everything, which is how a nonsense setting fails safe
    U64  mSessionCap;   // bytes; zero means no ceiling, which is never what the settings produce
    U64  mTokens;
    U64  mSpent;
    F64  mLastSecond;
    bool mStarted;
};

// ---------------------------------------------------------------------------
// The engine, in ssbc7promote.cpp
// ---------------------------------------------------------------------------

// Main thread. Resolves the store and caches the policy. Safe when the feature is off, in which case it does nothing at all and starts no threads - the fused work runs on the existing BC7Encode pool and this module never owns one.
void ssBC7PromoteInit();

// Main thread. Re-reads the SSSqueeze settings so the engine and its throttle can be changed without a restart, exactly as the encode and read gates already can.
void ssBC7PromoteRefreshPolicy();

// Main thread, every frame, and a clock comparison on almost all of them. Runs one pass at most every SSBC7_PROMOTE_TICK_SECONDS: drains tier (a), then tier (b) only when tier (a) has nothing left, then re-arms the read path for anything newly stored.
void ssBC7PromoteTick();

// Main thread. Stops issuing anything and cancels every continuation fetch this module owns, so a quit does not wait on a download nobody asked for.
void ssBC7PromoteBeginShutdown();
void ssBC7PromoteShutdown();

// The numbers worth putting in front of a person, as numbers rather than inside a log line, so the stats overlay can format and colour them. Wired up separately by the owner - nothing here edits ssstatsview.cpp.
struct SSBC7PromoteStats
{
    U32 mLocalPromoted     = 0;   // records created from J2C already on this disk, no network at all - tier (a)
    U32 mNetworkCompleted  = 0;   // records created after this engine completed a partial J2C over the network - tier (b)
    U32 mWaiting           = 0;   // want list depth right now, which is the honest answer to "how much is left"
    U32 mWantDropped       = 0;   // want-list entries the cap forced out this session; a non-zero value means the list is not a complete account of what is missing
    U32 mNetCandidates     = 0;   // partial textures held for network completion right now
    U32 mNetInFlight       = 0;
    U32 mNetIssued         = 0;
    U64 mNetBytesSpent     = 0;   // estimated, from the entry table's own totals rather than measured off the wire
    U64 mNetBytesCap       = 0;   // the whole-session ceiling, so a readout can show spent against allowed
    U32 mLastVerdict       = 0;   // ESSBC7PromoteVerdict - why the last pass ended the way it did
    // <SS:Nexii/> Squeeze adaptive quality - the third tier's own counters. The detailed readout of what is left to upgrade lives in SSBC7AdaptiveStats, which reads the store's histogram; these two are here so the promotion metrics line can say the tier ran at all.
    U32 mUpgradesPosted    = 0;   // re-encodes this engine handed to the pool this session
    U32 mUpgradeSkipped    = 0;   // candidates written off, almost always because their J2C has left this disk
    // <SS:Nexii/> Squeeze capacity-driven promotion - fused local encodes this engine has posted that have not finished: queued behind a worker plus running on one. The overlay shows it beside the busy-core count so "why is it only doing four" has a visible answer.
    U32 mLocalQueued       = 0;   // posted, no worker has started it yet
    U32 mLocalRunning      = 0;   // a worker is inside it now
};
SSBC7PromoteStats ssBC7PromoteStatsNow();

std::string ssBC7PromoteMetricsString();

#endif // SS_BC7PROMOTE_H
