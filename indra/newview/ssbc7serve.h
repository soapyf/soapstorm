/**
 * @file ssbc7serve.h
 * @brief Squeeze BC7 read path - probes the sidecar store, reads a mip prefix off the reader pool and uploads it in place of a J2C decode, see doc/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_BC7SERVE_H
#define SS_BC7SERVE_H

#include "lluuid.h"

#include <string>

class LLViewerFetchedTexture;

// The other half of ssbc7encodequeue: everything that decides whether a stored record is allowed to become the GL texture, the pool that reads it, and the main-thread queue that uploads it.
//
// THREE STAGES ON THREE DIFFERENT THREADS, and the split is forced by what each API locks rather than chosen. PROBE is a map find under the store's map mutex with no IO, so it is safe on the frame thread. READ is plain blocking fopen/fread holding no lock at all, so it must never be on the frame thread or on the texture fetch thread. UPLOAD is GL work and runs on the frame thread inside the existing per-frame texture-creation budget.

// Why a given texture was or was not served. Mirrors ESSBC7EncodeVerdict deliberately: a bare "return false" makes "the read path is off" and "the read path ran and refused everything" produce identical silence, which is the exact failure this project has already paid for once on the encode side.
enum ESSBC7ServeVerdict
{
    SSBC7_SERVE_HIT = 0,            // a record exists and every exclusion passed; the ladder moves to HIT_KNOWN
    SSBC7_SERVE_QUEUED,             // a prefix read is in flight
    SSBC7_SERVE_UPLOADED,           // terminal success - this is the verdict whose count is the feature working
    SSBC7_SERVE_DECLINE_OFF,        // SSSqueezeEnabled or SSSqueezeReadEnabled is off, or the GPU has no BPTC
    SSBC7_SERVE_DECLINE_NOT_READY,  // the store never came up, or shutdown has begun
    SSBC7_SERVE_DECLINE_NO_RECORD,  // nothing stored for this uuid, which is the ordinary answer on a cold cache
    SSBC7_SERVE_DECLINE_FTTYPE,     // not a plain fetched-by-id texture: a map tile, a server bake, a local file
    SSBC7_SERVE_DECLINE_SCULPT,     // sculpt maps are geometry, not colour, and a lossy codec moves vertices
    SSBC7_SERVE_DECLINE_UI,         // BOOST_UI textures do not discard and generally have no mip chain
    SSBC7_SERVE_DECLINE_ICON,       // BOOST_ICON and BOOST_THUMBNAIL are destructively rescaled by their own paths
    SSBC7_SERVE_DECLINE_LOCAL,      // file:// textures are rescaled in place by preCreateTexture
    SSBC7_SERVE_DECLINE_EXPLICIT,   // the caller demanded a GL format at construction, latched then because once we set our own the flag can no longer tell the two apart
    SSBC7_SERVE_DECLINE_AUX,        // mNeedsAux - a bump or sculpt source whose consumer wants the raw channels
    SSBC7_SERVE_DECLINE_RAW,        // needsToSaveRawImage, or a loaded callback that wants the LLImageRaw; serving would starve it
    SSBC7_SERVE_DECLINE_ALPHA,      // real alpha and the record carries no pick mask, so per-texel picking would regress to whole-quad. Since 2026-09-09 every encode of an alpha texture stores one, so this now only names a record from a store that predates that or lost its mask to a checksum failure
    SSBC7_SERVE_DECLINE_GEOMETRY,   // the record's dimensions or mip count do not satisfy the upload contract
    SSBC7_SERVE_DECLINE_BUSY,       // the reader queue refused the post, or an upload is already queued; retried on the next pass, never a block
    SSBC7_SERVE_DECLINE_READ,       // the blob did not read or did not verify; the record is already dropped and J2C takes over
    SSBC7_SERVE_DECLINE_STALE,      // an exclusion became true, or a better texture landed, between the post and the upload
    SSBC7_SERVE_DECLINE_UPLOAD,     // the GL upload itself refused
    SSBC7_SERVE_UPGRADED,           // a resident stepped back to the J2C path on purpose - a raw consumer arrived, or a better uncompressed image did
    SSBC7_SERVE_VERDICT_COUNT
};

const char* ssBC7ServeVerdictName(ESSBC7ServeVerdict verdict);

// Main thread. Resolves the store and caches the policy; safe to call when the feature is off, in which case it starts no threads at all.
void ssBC7ServeInit();

// Main thread. Re-reads the SSSqueeze settings so the read path can be switched on without a restart, and brings the reader pool up on demand.
void ssBC7ServeRefreshPolicy();

// Main thread. Stops accepting reads and tells any worker already inside one to give up at its next check.
void ssBC7ServeBeginShutdown();

// Main thread. Closes and joins the reader pool, then drops every completion still holding a texture reference.
void ssBC7ServeShutdown();

// Cheap enough to call per texture creation: one atomic load.
bool ssBC7ServeEnabled();

// STAGE 1, main thread, NO IO. NONE to HIT_KNOWN or to DECLINED. Called from LLViewerTextureList::createImage before fast-cache enrolment, because a 16x16 fast-cache raw arriving after a BC7 upload would clobber it.
ESSBC7ServeVerdict ssBC7ServeProbe(LLViewerFetchedTexture* tex);

// STAGE 2, main thread, posts to the reader pool. HIT_KNOWN to READING, or RESIDENT to READING when the wanted discard has changed. A refusal is never a block: the queue is tryPost only and a full queue simply means the next pass asks again.
ESSBC7ServeVerdict ssBC7ServeRequest(LLViewerFetchedTexture* tex, S32 desired_discard);

// STAGE 3, main thread. Drains completed reads and uploads them, re-validating every exclusion at the moment of upload because the texture can have been sculpt-flagged, boosted or handed a raw-needing callback since the post. Returns the time it spent.
F32 ssBC7ServePumpUploads(F32 max_time);

// Called by LLViewerFetchedTexture when a resident stops being one, so the live video-memory gauge is a gauge rather than a high-water mark.
void ssBC7ServeNoteResidencyLost(U32 bc7_bytes, U32 saved_bytes);

// Recorded for its own sake by callers that decline outside this module, so the tally stays the single answer to "what happened to the read path".
void ssBC7ServeRecord(ESSBC7ServeVerdict verdict);

// <SS:Nexii> Squeeze region manifests - how many prefix reads the shared pool is working on right now. Added so a background pre-warm can state "foreground fetch always wins" as an actual gate rather than as a hope: the manifest pass only ever adds to this pool when it has drained, which in a steady frame it has, because a prefix read is a fraction of a millisecond. Purely additive - nothing in the read path itself changed.
S32 ssBC7ServeReadsInFlight();
S64 ssBC7ServeReadBytesTotal();   // <SS:Nexii/> cumulative disk bytes read by the BC7 reader pool this session
// </SS:Nexii>

std::string ssBC7ServeMetricsString();

// <SS:Nexii> The two numbers worth putting in front of a person, as numbers rather than inside the log line. Video memory saved RIGHT NOW is the only figure that answers "is this feature doing anything for me", and a string is no use to an overlay that wants to format and colour it.
//
// Both are live totals over the currently resident set, not session running totals, so they fall when textures are released - which is correct for a readout of the present state and is why they are not simply the session counters.
struct SSBC7ServeResidency
{
    S64 mBC7Bytes    = 0;   // what the resident BC7 textures occupy
    S64 mSavedBytes  = 0;   // what they would have occupied uncompressed, minus the above
    U32 mTextures    = 0;
};
SSBC7ServeResidency ssBC7ServeResidencyNow();

#endif // SS_BC7SERVE_H
