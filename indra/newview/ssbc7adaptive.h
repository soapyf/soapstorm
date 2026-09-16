/**
 * @file ssbc7adaptive.h
 * @brief Squeeze adaptive encode quality - measures what the encode pool is actually achieving and picks the encoder profile that matches the backlog, see doc/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_BC7ADAPTIVE_H
#define SS_BC7ADAPTIVE_H

#include "ssbc7encoder.h"   // SSBC7Quality - newview sits above llimage, so this direction is the legal one

#include <string>

// WHAT THIS IS FOR. The encode pool is SUPPLY limited rather than compute limited: sixteen background workers fed by a network that delivers a texture every few hundred milliseconds spend most of their time waiting, which is exactly why spending more CPU per texture is nearly free. So the normal state is "encode at the best profile", and the interesting question is only ever the exceptional one - is the machine, right now, failing to drain what is waiting for it. This module answers that from MEASUREMENT rather than from a guess about the hardware.
//
// WHY IT CAN EXIST AT ALL. Quality used to be folded into ssBC7BlockBackendVersion(), and a version mismatch WIPES the store - so a profile that varied within a session would have destroyed the cache several times an hour. The profile now lives in SSBC7Record::mQuality, which is what makes a store holding a mixture legal, and that in turn makes the second half of this feature possible: a record encoded in a hurry is not wrong, it is unfinished, and the idle upgrade pass goes back and re-encodes it properly.

// ---------------------------------------------------------------------------
// Constants - the whole control law's tuning, in one place a person can read
// ---------------------------------------------------------------------------

constexpr U32 SSBC7_ADAPT_TICK_SECONDS     = 2;    // how often the controller may reconsider; on every other frame its whole cost is a clock comparison
constexpr U32 SSBC7_ADAPT_WINDOW_SECONDS   = 60;   // the moving window the rate is measured over, long enough that one unusually large texture does not move it and short enough to notice a region change
constexpr U32 SSBC7_ADAPT_WINDOW_SAMPLES   = 64;   // encodes remembered per profile; the window is whichever of the two bounds bites first, which is what keeps a rate available after a quiet minute
constexpr U32 SSBC7_ADAPT_MIN_SAMPLES      = 4;    // below this the window is too thin to believe, so the older samples in the ring are used instead of reporting nothing

// THE DEAD BAND, and it is the reason this cannot oscillate. Both numbers are estimated seconds to drain the current backlog: stepping DOWN needs the CURRENT profile to be over the slow bar, stepping UP needs the profile ONE RUNG BETTER to be predicted under the fast bar. Because the up test is made against the target rather than the current rate, a step up cannot immediately undo itself - the prediction that justified it is the same quantity the step-down test will apply a moment later, and it is three times further from the bar.
constexpr F32 SSBC7_ADAPT_DRAIN_SLOW_SECONDS = 300.f;
constexpr F32 SSBC7_ADAPT_DRAIN_FAST_SECONDS = 90.f;

// THE DWELL, and it is deliberately asymmetric. Falling behind is acted on in twenty seconds because the cost of being late is that the user waits; catching up is acted on in ninety because the cost of being early is a burst of records at the wrong profile that the upgrade pass then has to redo. The cooldown is on top of both: whatever the signal says, no two changes may be closer together than this.
constexpr U32 SSBC7_ADAPT_DOWN_DWELL_SECONDS = 20;
constexpr U32 SSBC7_ADAPT_UP_DWELL_SECONDS   = 90;
constexpr U32 SSBC7_ADAPT_COOLDOWN_SECONDS   = 60;

// Seeds for a profile nothing has been encoded at yet, in megapixels per second PER WORKER, straight off the offline benchmark at 512x512 single threaded. They exist only so the very first decision of a session is not made against zeroes; the first few real encodes replace them, which matters because these were measured on synthetic content and the machine that counts is the one the user is sat at.
//
// These are bc7f's Faster, Fast and Default presets on textured content (the whole-image figure, which excludes the mostly-solid cutout class that every encoder short-circuits). bc7f is analytical, so its cost is content dependent: smooth content runs two to three times faster than these, a block of several unrelated hues several times slower.
constexpr F32 SSBC7_ADAPT_SEED_MPIX_FAST     = 110.f;
constexpr F32 SSBC7_ADAPT_SEED_MPIX_BALANCED = 89.f;
constexpr F32 SSBC7_ADAPT_SEED_MPIX_HIGH     = 52.f;

// A texture whose size nothing has measured yet. A 1024 square is 1.05 megapixels, and the mip chain adds about a third, so this is the honest starting guess for the sort of asset the backlog is made of.
constexpr F32 SSBC7_ADAPT_SEED_MPIX_PER_TEXTURE = 1.4f;

// Upgrade candidates handed to one idle pass. Small on purpose: each one is a J2C read, a full decode and a full encode, and the pass must be able to stop the instant the user does anything.
constexpr size_t SSBC7_ADAPT_UPGRADE_PER_PASS = 4;

// Uuids the upgrade pass has already failed on, remembered so a texture whose J2C has left this disk is not fetched, decoded and refused once every pass for the rest of the session.
constexpr size_t SSBC7_ADAPT_UPGRADE_SKIP_CAP = 4096;

// ---------------------------------------------------------------------------
// Why the controller is where it is
// ---------------------------------------------------------------------------

// A user seeing a lower profile than they expected needs to know whether the viewer chose it or they did, so the reason travels with the choice rather than being reconstructed from a log.
enum ESSBC7AdaptReason
{
    SSBC7_ADAPT_PINNED = 0,     // SSSqueezeEncodeQuality names a profile, so nothing here may override it - the user's setting always wins
    SSBC7_ADAPT_NO_DATA,        // adaptive, but nothing has been encoded yet, so the ladder is sitting at the best profile on the seeded figures
    SSBC7_ADAPT_HEADROOM,       // at the best profile and keeping up with room to spare, which is the state this feature expects to be in almost always
    SSBC7_ADAPT_HOLDING,        // no sustained signal in either direction; inside the dead band on purpose
    SSBC7_ADAPT_STEPPED_DOWN,   // sustained backlog: the profile was lowered to drain it
    SSBC7_ADAPT_STEPPED_UP,     // the backlog cleared and a better profile is predicted to keep up
    SSBC7_ADAPT_NO_BACKEND,     // the linked block backend has one encoder, so there is no ladder to climb
    SSBC7_ADAPT_REASON_COUNT
};

const char* ssBC7AdaptReasonName(ESSBC7AdaptReason reason);

// ---------------------------------------------------------------------------
// Pure policy, in ssbc7adaptivepolicy.cpp - no viewer globals, no threads, no clock of its own, so the offline harness exercises exactly the code that ships
// ---------------------------------------------------------------------------

// A moving window of completed encodes, per profile. Per profile because the ladder's whole purpose is knowing what a step down would actually BUY, and a single blended rate cannot answer that - it would report the average of two profiles and call it either one.
//
// NOT thread safe. The encode module owns one behind a mutex, taken once per completed texture rather than anywhere near the block loop, which is a cost of about a microsecond against an encode of a hundred milliseconds.
class SSBC7RateWindow
{
public:
    SSBC7RateWindow();

    // One completed encode. `mpix` is the texels the backend was actually handed, padding and mips included, because that is the work the machine really did; `seconds` is that one worker's wall time, so the rate this yields is PER WORKER and the pool's capacity is it multiplied by the width.
    void add(SSBC7Quality quality, F32 mpix, F32 seconds, F64 now);

    // Megapixels per second per worker for one profile, or zero when nothing has been encoded at it. Samples inside the window are preferred; if there are too few of them the whole ring is used regardless of age, because a stale measurement of a real machine beats a benchmark figure from a different one.
    F32 rate(SSBC7Quality quality, F64 now) const;

    // How many samples the answer above was built from, so a readout can say whether a figure is measured or seeded.
    U32 samples(SSBC7Quality quality, F64 now) const;

    // What the machine is ACHIEVING across all workers, as opposed to what one worker is capable of: total megapixels in the window divided by the window's own span. Idle workers pull this down, which is correct - it is the number that answers "how fast is my cache filling right now".
    F32 aggregateRate(F64 now) const;

    // Mean megapixels per encoded texture, which is what converts a backlog counted in textures into a backlog counted in work. Falls back to the seed until anything has been encoded.
    F32 meanMpixPerTexture(F64 now) const;

    F64 busySeconds() const { return mBusySeconds; }   // cumulative worker seconds spent inside an encode, monotonic, so an overlay can difference it across frames and get an occupancy without this module having to guess the frame rate
    U32 encodes() const { return mEncodes; }

private:
    struct Sample
    {
        F64 mWhen;
        F32 mMpix;
        F32 mSeconds;
    };

    Sample mRing[SSBC7_QUALITY_COUNT][SSBC7_ADAPT_WINDOW_SAMPLES];
    U32    mFilled[SSBC7_QUALITY_COUNT];   // how many slots of the ring are valid, capped at the ring size
    U32    mNext[SSBC7_QUALITY_COUNT];     // where the next sample goes
    F64    mBusySeconds;
    U32    mEncodes;
};

// Everything the ladder is allowed to look at. Assembled by the caller so the decision itself has no clock, no settings and no store - which is what lets the offline harness drive a whole session past it in a few microseconds.
struct SSBC7LadderInput
{
    F64          mNow             = 0.0;    // monotonic seconds; the ladder never reads a clock of its own
    U32          mBacklog         = 0;      // textures waiting: the want list plus whatever is already in flight
    F32          mMpixPerTexture  = SSBC7_ADAPT_SEED_MPIX_PER_TEXTURE;
    // Pool CAPACITY at each profile in megapixels per second - the per-worker measured rate multiplied by the number of workers. Capacity rather than achieved throughput, because the question is whether a profile COULD drain the backlog, not whether it happens to be doing so while the network is quiet.
    F32          mCapacityMpixPerSec[SSBC7_QUALITY_COUNT] = {0.f, 0.f, 0.f};
    bool         mPinned          = false;  // the user named a profile, which always wins
    SSBC7Quality mPinnedQuality   = SSBC7_QUALITY_HIGH;
};

// The control law. One rung per profile, best first, ordered by MEASURED cost rather than by the profile names.
class SSBC7QualityLadder
{
public:
    SSBC7QualityLadder();

    // Puts the ladder at a known rung and clears every timer, which is what a settings change or a fresh session wants.
    void reset(SSBC7Quality quality, F64 now);

    // One decision. Returns true when the profile changed, so the caller logs exactly the transitions and nothing else.
    bool update(const SSBC7LadderInput& in);

    SSBC7Quality      quality() const { return mQuality; }
    ESSBC7AdaptReason reason() const  { return mReason; }
    U32               changes() const { return mChanges; }

    // Estimated seconds to clear the backlog at the profile currently in force, which is the single number the whole law turns on. Zero when there is no backlog, and negative when it could not be estimated at all.
    F32 drainSeconds() const { return mDrainSeconds; }

    // Seconds the current directional signal has been unbroken, for a readout that wants to show a decision coming rather than only announcing it afterwards.
    F32 behindFor(F64 now) const;
    F32 comfortableFor(F64 now) const;

private:
    SSBC7Quality      mQuality;
    ESSBC7AdaptReason mReason;
    F32               mDrainSeconds;
    F64               mBehindSince;        // when the "falling behind" condition last became true; zero when it is not true now
    F64               mComfortableSince;
    F64               mLastChange;
    U32               mChanges;
};

// Estimated seconds to drain `backlog` textures at a given capacity. Exposed because it is the one piece of arithmetic in the law and it is worth being able to pin it in a test rather than inferring it from a decision. Returns -1 when the capacity is unusable, which every caller must treat as "unknown" rather than as "instant".
F32 ssBC7AdaptDrainSeconds(U32 backlog, F32 mpix_per_texture, F32 capacity_mpix_per_sec);

// ---------------------------------------------------------------------------
// The live controller, in ssbc7adaptive.cpp
// ---------------------------------------------------------------------------

// Main thread. Reads the settings and puts the ladder at its starting rung. Safe when Squeeze is off, in which case it starts nothing.
void ssBC7AdaptiveInit();

// Main thread. Re-reads SSSqueezeEncodeQuality and SSSqueezeUpgradeIdle so pinning or unpinning a profile takes effect without a restart.
void ssBC7AdaptiveRefreshPolicy();

// ANY THREAD, and one relaxed atomic load. This is what the encode path calls for every texture, which is why it must never touch a mutex or a setting.
SSBC7Quality ssBC7AdaptiveQualityNow();

// Worker threads. One completed encode: the texels the backend was handed and the wall time it took. Takes a short mutex, once per texture, never inside the block loop.
void ssBC7AdaptiveNoteEncode(SSBC7Quality quality, U32 texels, F64 seconds);

// Worker threads. Brackets the time a worker spends inside an encode, so the overlay can say how many of the cores are actually busy rather than how many exist.
void ssBC7AdaptiveNoteBusy(bool busy);

// <SS:Nexii> Squeeze capacity-driven promotion - the INSTANT count of workers inside an encode, as against the moving average the overlay shows. The promotion engine sizes each pass from this: pool width minus this minus what it has already queued is the number of cores it may put to work right now, and an average would let it over-post on the way up and under-post on the way down.
S32 ssBC7AdaptiveBusyWorkersNow();
// </SS:Nexii>

// Main thread, every frame, and a clock comparison on almost all of them. Feeds the ladder and logs every transition.
void ssBC7AdaptiveTick();

// The profile the idle upgrade pass should aim at, which is the best rung on the ladder. Separate from ssBC7AdaptiveQualityNow because the upgrade pass only ever runs when the machine is idle and must therefore always aim high, never at whatever a busy minute talked the controller into.
SSBC7Quality ssBC7AdaptiveUpgradeTarget();

// True when re-encoding old records is permitted at all: the setting is on, the backend has more than one profile, and the controller is not currently stepped down - because a machine that cannot keep up with new work has no business redoing old work.
bool ssBC7AdaptiveUpgradeAllowed(std::string& out_reason);

// Any thread. The promotion engine brackets each piece of upgrade work with this, purely so the overlay can say "improving old textures" while it happens rather than only reporting a total afterwards. Calls NEST - the scan raises it and each re-encode it posts raises it again - so every true must be matched by exactly one false.
void ssBC7AdaptiveNoteUpgradeRunning(bool running);

// Any thread. One upgrade candidate the pass could not re-encode, almost always because its J2C has since left this disk. Counted rather than logged per texture, because a store full of records whose sources are gone would otherwise write a line each, every pass, forever.
void ssBC7AdaptiveNoteUpgradeFailed();

// ---------------------------------------------------------------------------
// The readout
// ---------------------------------------------------------------------------

// Numbers rather than a formatted string, in the style of SSBC7PromoteStats, so the stats overlay colours and lays them out itself. Wired up separately by the owner - nothing here edits ssstatsview.cpp.
struct SSBC7AdaptiveStats
{
    // ---- which profile, and why it is at that one ----
    U8   mQuality          = (U8)SSBC7_QUALITY_HIGH;   // SSBC7Quality now in force; name it with ssBC7QualityName
    U8   mReason           = (U8)SSBC7_ADAPT_NO_DATA;  // ESSBC7AdaptReason; name it with ssBC7AdaptReasonName
    bool mAdaptive         = false;                    // false means the user pinned a profile and nothing here may move it
    U32  mChanges          = 0;                        // profile changes this session, so a readout can show the controller is settled rather than thrashing

    // ---- how the cores are being used ----
    U32  mWorkers          = 0;    // pool width, zero when the pool has not started
    F32  mWorkersBusy      = 0.f;  // MOVING AVERAGE of workers inside an encode, not an instant sample: on a supply limited pool an instant sample reads zero most of the time and would look broken
    F64  mBusySecondsTotal = 0.0;  // cumulative worker seconds inside an encode, monotonic, for an overlay that would rather difference it across frames than trust this module's smoothing

    // ---- the measured rate ----
    F32  mAggregateMpixPerSec = 0.f;   // what the machine is ACHIEVING across all workers right now, which is the one number to put in front of a person
    F32  mProfileMpixPerSec[SSBC7_QUALITY_COUNT] = {0.f, 0.f, 0.f};   // per worker, per profile, measured; zero where nothing has been encoded at that profile yet
    U32  mProfileSamples[SSBC7_QUALITY_COUNT]    = {0, 0, 0};         // how many encodes each of those figures rests on, so a readout can mark a thin one
    F32  mCapacityMpixPerSec  = 0.f;   // per-worker rate at the current profile times the worker count - what the pool COULD drain, as against what it is
    U32  mBacklog             = 0;     // textures waiting: want list plus in flight
    F32  mDrainSeconds        = 0.f;   // estimated seconds to clear that backlog at the current profile; negative means it could not be estimated

    // ---- the upgrade pass ----
    U32  mUpgraded         = 0;      // records re-encoded at a better profile this session
    U32  mBelowBest        = 0;      // records still sitting below the best profile - the denominator without which the number above means nothing
    bool mUpgradeRunning   = false;  // an upgrade pass is in flight right now, so the overlay can say "improving old textures" rather than only reporting totals
    U32  mUpgradeFailed    = 0;      // candidates the pass gave up on, almost always because the J2C has since left this disk
    U32  mRecordsAtQuality[SSBC7_QUALITY_COUNT] = {0, 0, 0};   // histogram, maintained incrementally by the store, so reading it costs a counter copy and never a scan
};

SSBC7AdaptiveStats ssBC7AdaptiveStatsNow();

std::string ssBC7AdaptiveMetricsString();

#endif // SS_BC7ADAPTIVE_H
