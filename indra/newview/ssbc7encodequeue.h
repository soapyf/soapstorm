/**
 * @file ssbc7encodequeue.h
 * @brief Squeeze BC7 encode pool - the demand-path hook that turns a freshly decoded full resolution texture into a stored sidecar, see doc/viewer/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_BC7ENCODEQUEUE_H
#define SS_BC7ENCODEQUEUE_H

#include "llpointer.h"
#include "lluuid.h"
#include "llviewertexture.h"    // FTType
#include "ssbc7encoder.h"       // <SS:Nexii/> Squeeze adaptive quality - SSBC7Quality is now a parameter of an encode, so the seam the promotion engine calls through has to name it

#include <functional>
#include <string>
#include <vector>

class LLImageRaw;

// Everything that decides whether a decoded texture becomes a BC7 record, and the pool that does the work.
//
// This is deliberately a set of free functions over one file-static state object rather than an LLSingleton: the consider entry point is called from the texture fetch thread, and LLSingleton's first-touch construction is a main-thread affair. The state is created once on the main thread and then never destroyed, so a fetch thread reading it during shutdown finds a closed pool rather than freed memory.

// Why a given texture was or was not enqueued. Every one of these is counted and periodically logged, because a bare rejection makes "the feature is off" and "the feature ran and declined everything" look identical from a log file - which is precisely the failure this project has already paid for once.
enum ESSBC7EncodeVerdict
{
    SSBC7_ENQUEUED = 0,
    SSBC7_DECLINE_OFF,              // SSSqueezeEnabled or SSSqueezeBackgroundEncode is off, or the GPU has no BPTC so there would be nothing to serve the record to
    SSBC7_DECLINE_NOT_READY,        // the store never came up, is a read-only second instance, or shutdown has begun
    SSBC7_DECLINE_TYPE,             // an excluded texture kind - see doc/viewer/super_compressed_textures.md for why each one is excluded
    SSBC7_DECLINE_PARTIAL,          // not full resolution, which is the whole point of full-res-only; the uuid goes on the want list for the promotion engine instead
    SSBC7_DECLINE_GEOMETRY,         // non power of two, below the minimum size, or a component count BC7 cannot represent
    SSBC7_DECLINE_ALREADY,          // already stored, or already being encoded by another worker
    SSBC7_DECLINE_BACKPRESSURE,     // the queue or the pinned-bytes budget is full; lossless, since the J2C is on disk and the promotion engine will come back for it
    SSBC7_DECLINE_POST_FAILED,      // the pool refused the work item
    SSBC7_VERDICT_COUNT
};

const char* ssBC7VerdictName(ESSBC7EncodeVerdict verdict);

// Main thread. Brings up the pool and caches the policy that the fetch thread reads; safe to call when the feature is off, in which case it does nothing at all.
void ssBC7EncodeInit();

// Main thread. Re-reads the SSSqueeze settings into the cached policy so the gate can be flipped without a restart. The fetch thread only ever reads these cached values, never gSavedSettings.
void ssBC7EncodeRefreshPolicy();

// Main thread. Stops accepting work and tells any worker already inside an encode to give up at its next check. Encodes are ABANDONED rather than drained at quit: the work queue drains itself before it closes, so without an abandon flag a full queue of megapixel encodes would be executed in full while the user waits for the window to disappear.
void ssBC7EncodeBeginShutdown();

// Main thread. Closes and joins the pool. Must run BEFORE SSBC7Store::shutdownStore(), because every in-flight append belongs to a worker that this call is what waits for. Idempotent.
void ssBC7EncodeShutdown();

// Any thread, in practice the texture fetch thread from the DONE state. Applies every gate and either posts the encode or records why it did not.
//
// MUST NOT BLOCK. The caller holds LLTextureFetchWorker::mWorkMutex for the whole of doWork, so anything that parked here would park every texture in the world behind it: the post is a tryPost, the store lookup takes only the store's map lock, and no file IO happens on this thread at all. A verdict other than SSBC7_ENQUEUED always means the texture carries on down the ordinary uncompressed path exactly as it does today.
ESSBC7EncodeVerdict ssBC7EncodeConsider(const LLUUID& id,
                                        FTType ftype,
                                        const LLPointer<LLImageRaw>& raw,
                                        S32 decoded_discard,
                                        bool have_all_data,
                                        bool needs_aux,
                                        bool in_local_cache);

// <SS:Nexii> Squeeze eviction - main thread, every frame, and almost always a clock comparison and nothing else. It owns the two jobs that have to happen on the frame thread and nowhere else: marking every texture the running viewer still references, which is the only way a permanently resident texture ever looks warm, and posting the eviction pass onto THIS pool rather than a second one - a WorkQueue post made from inside the store's own lock is how a deadlock gets written, so appendBlob only ever raises a flag and this is what reads it.
void ssBC7EncodeMaintenanceTick();
// </SS:Nexii>

// Snapshot of the want list, which is where every uuid that was declined for a reason a later pass could fix ends up. The promotion engine in ssbc7promote.cpp drains it; the snapshot form remains so the population can be inspected without disturbing it.
void ssBC7EncodeWantList(std::vector<LLUUID>& out);
size_t ssBC7EncodeWantListSize();

// <SS:Nexii> Squeeze promotion - the seam ssbc7promote.cpp runs on. It is a set of small accessors rather than a second queue on purpose: promotion work has to share the DEMAND path's pool, its bounded queue, its pinned-bytes budget and its in-flight dedupe set, because two pools would mean a backfill encode could sit ahead of a texture the user is looking at, and two budgets would mean the 256 MB ceiling on pinned raws is really 512 MB.

// True when everything the encode side needs is up: the feature is on, the GPU can actually use a record, and the store is initialised and writable. `out_reason` is filled in on refusal and is what the promotion engine's own decline verdict is built from.
bool ssBC7EncodeGateReady(std::string& out_reason);

// The same geometry rule ssBC7EncodeConsider applies, exposed so the promotion engine rejects a texture BEFORE spending a decode on it rather than after.
bool ssBC7EncodeGeometryOK(U32 width, U32 height, U32 components);

// Demand-path encodes still in flight. The promotion engine only runs when this is zero, which is the whole of "foreground always wins" on the CPU side.
S32 ssBC7EncodePendingCount();

// <SS:Nexii/> Squeeze adaptive quality - how many workers the pool actually has, or zero before it starts. The controller multiplies the measured per-worker rate by this to get the pool's capacity, which is the only sense in which "would this profile keep up" has an answer; reading the setting instead would be wrong on every machine where the setting is left at its automatic zero.
S32 ssBC7EncodePoolWidth();

// Set once shutdown starts. A long-running promotion item polls it between textures so quit does not wait on a backfill nobody asked for.
bool ssBC7EncodeAbandonRequested();

// Plain-uuid dedupe across the demand path and the promotion engine, which is what stops the two from encoding the same texture at the same time and throwing one of the results away. A successful claim also takes the uuid off the want list.
bool ssBC7EncodeClaim(const LLUUID& id);
void ssBC7EncodeUnclaim(const LLUUID& id);

// The pinned-raw budget. Reserve BEFORE decoding, because the decode is what allocates the bytes being accounted for, and release on every exit including failure - a promotion item that leaked its reservation would leave the demand path refusing everything for the rest of the session.
bool ssBC7EncodeReserveBytes(S64 bytes);
void ssBC7EncodeReleaseBytes(S64 bytes);

// tryPost onto the shared pool. NEVER blocks; false means the queue is full or closed and the caller retries at its next tick.
bool ssBC7EncodeTryPost(const std::function<void()>& work);

// Worker threads only. Encodes an already decoded full-resolution raw and appends it to the store, and is the single implementation the demand path and the promotion engine both use. Returns false and logs the reason on any failure.
//
// <SS:Nexii> Squeeze adaptive quality - `quality` is the profile to encode at, chosen by the caller rather than read here, because the two callers want different answers: the demand path wants whatever the controller has settled on this second, while the idle upgrade pass always wants the best rung. `allow_supersede` is what lets that pass replace an existing record; it is false everywhere else, so the plain-uuid dedupe the demand path relies on is unchanged.
bool ssBC7EncodeAndStore(const LLUUID& id, const LLPointer<LLImageRaw>& raw, SSBC7Quality quality, bool allow_supersede);

// Want-list access for the engine. take() hands back the NEWEST entries and removes them, so two passes never chase the same texture; want() puts one back or adds a new one and is the same call the decline paths use.
size_t ssBC7EncodeTakeWanted(size_t max_count, std::vector<LLUUID>& out);
void   ssBC7EncodeWant(const LLUUID& id);

// How many uuids the want list's cap has thrown away this session. Non-zero means the number still waiting is a FLOOR and not a total, which is the difference between an honest readout and one that claims the engine has everything in hand.
U32 ssBC7EncodeWantDroppedTotal();

// Uuids that reached the store this session, drained by the main thread. Without this a texture probed before its record existed stays DECLINED for NO_RECORD until the viewer restarts, which would quietly throw away the video memory the promotion engine just spent CPU to make available.
void ssBC7EncodeTakeFreshlyStored(std::vector<LLUUID>& out);
// </SS:Nexii>

std::string ssBC7EncodeMetricsString();

#endif // SS_BC7ENCODEQUEUE_H
