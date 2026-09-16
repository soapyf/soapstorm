/**
 * @file ssbc7adaptive.cpp
 * @brief Squeeze adaptive encode quality - the live controller: measurement from the workers, one decision every couple of seconds on the main thread, see doc/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssbc7adaptive.h"

#include "llviewercontrol.h"
#include "ssbc7encodequeue.h"
#include "ssbc7store.h"

#include <atomic>
#include <chrono>
#include <mutex>

// THE SHAPE, and why each half runs where it does.
//
// The MEASUREMENT is written by encode workers, once per completed texture, under a mutex held for about a microsecond against an encode of a hundred milliseconds. The DECISION runs on the main thread every couple of seconds and is the only thing that ever writes the profile. The READ - the one call the encode path makes per texture - is a single relaxed atomic load and touches nothing else, because it happens with the texture fetch worker's own lock held and anything that parked there would park every texture in the world behind it.
//
// Nothing here can block the main thread and nothing here can block a fetch thread. The worst case anywhere in this file is one uncontended mutex acquisition on a pool worker that has just spent a hundred milliseconds in the vector units.

namespace
{
    // A monotonic clock of this module's own, in seconds since the first call. steady_clock rather than LLTimer because this is read from pool workers and from the main thread and must never be able to go backwards - a negative duration would divide into an infinite rate and then justify the best profile forever.
    F64 nowSeconds()
    {
        static const std::chrono::steady_clock::time_point s_origin = std::chrono::steady_clock::now();
        return std::chrono::duration<F64>(std::chrono::steady_clock::now() - s_origin).count();
    }

    struct AdaptiveState
    {
        // ---- read from every thread ----
        // THE HOT PATH, and the only thing the encode path reads. Relaxed because a texture encoded at the profile from a fraction of a second ago is not a bug: the profile it was actually encoded at travels with it into the record, so there is nothing for this value to be inconsistent WITH.
        std::atomic<U32>  mQuality{(U32)SSBC7_QUALITY_HIGH};
        std::atomic<bool> mAdaptive{false};
        std::atomic<bool> mUpgradeEnabled{true};
        std::atomic<bool> mBackendHasProfiles{true};
        std::atomic<S32>  mBusyWorkers{0};
        // A COUNT rather than a flag, because the pass is a scan that then posts encodes: a bool raised by the scan and lowered when it returns would read false for most of the time the re-encodes are actually running, and the overlay would blink rather than say "improving old textures".
        std::atomic<S32>  mUpgradeRunning{0};
        std::atomic<U32>  mUpgradeFailed{0};

        // ---- the window, written by workers and read by the tick ----
        std::mutex        mWindowMutex;
        SSBC7RateWindow   mWindow;

        // ---- main thread only ----
        SSBC7QualityLadder mLadder;
        SSBC7Quality       mPinned{SSBC7_QUALITY_HIGH};
        F64                mNextTick{0.0};
        F32                mBusyAverage{0.f};
        F32                mLastCapacity{0.f};   // the capacity figure the last decision was made against, kept so the readout shows the number the controller actually used rather than one recomputed a second later
        bool               mWarnedNoProfiles{false};
    };

    // Created once on the main thread and then never destroyed, for the same reason the encode queue's state is: a pool worker that loads this pointer while the viewer is quitting has to find a stopped controller rather than freed memory.
    std::atomic<AdaptiveState*> s_state{nullptr};

    AdaptiveState* state() { return s_state.load(std::memory_order_acquire); }

    // Reads the user's setting into the two things it actually decides. Values 0 to 2 name a profile and pin it; anything else - which in practice means the default of 3 - asks for adaptive selection. Expressed as an extra VALUE rather than as a second checkbox so that "which profile am I getting" stays one question with one answer, and so that a user who pinned HIGH in a previous build keeps HIGH.
    void readQualitySetting(bool& out_adaptive, SSBC7Quality& out_pinned)
    {
        const U32 q = gSavedSettings.getU32("SSSqueezeEncodeQuality");
        if (q < (U32)SSBC7_QUALITY_COUNT)
        {
            out_adaptive = false;
            out_pinned   = (SSBC7Quality)q;
            return;
        }
        out_adaptive = true;
        out_pinned   = SSBC7_QUALITY_HIGH;
    }

    // The best rung on the ladder, which is what the upgrade pass always aims at and what a fresh session starts from.
    SSBC7Quality bestQuality() { return SSBC7_QUALITY_HIGH; }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ssBC7AdaptiveInit()
{
    if (state()) return;

    AdaptiveState* st = new AdaptiveState();
    st->mBackendHasProfiles = ssBC7BackendHasQualityProfiles();
    s_state.store(st, std::memory_order_release);

    ssBC7AdaptiveRefreshPolicy();
}

void ssBC7AdaptiveRefreshPolicy()
{
    AdaptiveState* st = state();
    if (!st) return;

    bool adaptive = false;
    SSBC7Quality pinned = SSBC7_QUALITY_HIGH;
    readQualitySetting(adaptive, pinned);

    // A backend with one encoder cannot climb a ladder, so adaptive selection on it would be a controller changing a number nothing reads. Said once, at INFO, rather than silently behaving as though the feature were on.
    if (adaptive && !st->mBackendHasProfiles.load())
    {
        adaptive = false;
        pinned   = bestQuality();
        if (!st->mWarnedNoProfiles)
        {
            st->mWarnedNoProfiles = true;
            LL_INFOS("Squeeze") << "BC7 adaptive quality is inert: the linked block backend \"" << ssBC7BlockBackendName()
                                << "\" has a single encoder, so there is no profile to select between and every record will be written at the one it has" << LL_ENDL;
        }
    }

    st->mAdaptive       = adaptive;
    st->mPinned         = pinned;
    st->mUpgradeEnabled = gSavedSettings.getBOOL("SSSqueezeUpgradeIdle");

    const F64 now = nowSeconds();
    const SSBC7Quality start = adaptive ? bestQuality() : pinned;
    st->mLadder.reset(start, now);
    st->mQuality.store((U32)start, std::memory_order_relaxed);

    LL_INFOS("Squeeze") << "BC7 encode quality: " << (adaptive ? "adaptive" : "pinned by the user")
                        << ", starting at " << ssBC7QualityName(start)
                        << ", idle re-encode of old records " << (st->mUpgradeEnabled.load() ? "on" : "off") << LL_ENDL;
}

// ---------------------------------------------------------------------------
// The hot path and the measurement
// ---------------------------------------------------------------------------

SSBC7Quality ssBC7AdaptiveQualityNow()
{
    AdaptiveState* st = state();
    // The best profile is the answer when the controller never came up, which is the same answer the viewer gave before this feature existed. A missing controller must never mean "encode badly".
    if (!st) return bestQuality();

    const U32 q = st->mQuality.load(std::memory_order_relaxed);
    return (q < (U32)SSBC7_QUALITY_COUNT) ? (SSBC7Quality)q : bestQuality();
}

SSBC7Quality ssBC7AdaptiveUpgradeTarget()
{
    // Always the best rung, never the profile currently in force. The upgrade pass only runs when the machine is idle, so aiming it at a profile some busy minute talked the controller into would have it rewrite records at the same quality they already have.
    return bestQuality();
}

void ssBC7AdaptiveNoteEncode(SSBC7Quality quality, U32 texels, F64 seconds)
{
    AdaptiveState* st = state();
    if (!st) return;

    const F32 mpix = (F32)((F64)texels / 1000000.0);

    std::lock_guard<std::mutex> lock(st->mWindowMutex);
    st->mWindow.add(quality, mpix, (F32)seconds, nowSeconds());
}

void ssBC7AdaptiveNoteBusy(bool busy)
{
    AdaptiveState* st = state();
    if (!st) return;

    if (busy) ++st->mBusyWorkers;
    else      --st->mBusyWorkers;
}

// <SS:Nexii/> Squeeze capacity-driven promotion - one atomic read; clamped because the decrement in a scope guard can briefly race the increment of another worker and read minus one.
S32 ssBC7AdaptiveBusyWorkersNow()
{
    AdaptiveState* st = state();
    return st ? llmax(0, st->mBusyWorkers.load()) : 0;
}

void ssBC7AdaptiveNoteUpgradeRunning(bool running)
{
    AdaptiveState* st = state();
    if (!st) return;

    if (running) ++st->mUpgradeRunning;
    else         --st->mUpgradeRunning;
}

void ssBC7AdaptiveNoteUpgradeFailed()
{
    AdaptiveState* st = state();
    if (!st) return;
    ++st->mUpgradeFailed;
}

bool ssBC7AdaptiveUpgradeAllowed(std::string& out_reason)
{
    AdaptiveState* st = state();
    if (!st)                              { out_reason = "the adaptive controller never initialised"; return false; }
    if (!st->mUpgradeEnabled.load())      { out_reason = "SSSqueezeUpgradeIdle is off"; return false; }
    if (!st->mBackendHasProfiles.load())  { out_reason = "this block backend has one encoder, so there is nothing to upgrade to"; return false; }

    // A machine that has just been told it cannot keep up with NEW work has no business redoing OLD work, whatever the promotion engine's own idle rule says. This is the second half of the control law rather than a separate policy: the ladder being off its best rung IS the statement that the CPU is short.
    if (ssBC7AdaptiveQualityNow() != bestQuality())
    {
        out_reason = "the controller has stepped down, so the machine is behind on new work and old work waits";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The decision
// ---------------------------------------------------------------------------

void ssBC7AdaptiveTick()
{
    AdaptiveState* st = state();
    if (!st) return;

    const F64 now = nowSeconds();
    if (now < st->mNextTick) return;
    st->mNextTick = now + (F64)SSBC7_ADAPT_TICK_SECONDS;

    // A MOVING AVERAGE OF OCCUPANCY, NOT AN INSTANT SAMPLE. The pool is supply limited, so an instant sample taken on the main thread lands between fetches most of the time and reads zero - which on a machine that is quietly encoding all day would look broken. A time constant of roughly ten seconds is short enough to follow a teleport arrival and long enough not to flicker.
    const F32 busy_now = (F32)llmax(0, st->mBusyWorkers.load());
    st->mBusyAverage = st->mBusyAverage * 0.8f + busy_now * 0.2f;

    const U32 workers = (U32)llmax(0, ssBC7EncodePoolWidth());
    const U32 backlog = (U32)ssBC7EncodeWantListSize() + (U32)llmax(0, ssBC7EncodePendingCount());

    SSBC7LadderInput in;
    in.mNow     = now;
    in.mBacklog = backlog;

    {
        std::lock_guard<std::mutex> lock(st->mWindowMutex);
        in.mMpixPerTexture = st->mWindow.meanMpixPerTexture(now);

        for (U32 q = 0; q < (U32)SSBC7_QUALITY_COUNT; ++q)
        {
            F32 per_worker = st->mWindow.rate((SSBC7Quality)q, now);
            if (!(per_worker > 0.f))
            {
                // Nothing has been encoded at this profile yet, so the benchmark seed stands in. It is only ever used to answer "what would the OTHER rung do", which is a question with no measurement available by definition the first time it is asked.
                switch (q)
                {
                    case SSBC7_QUALITY_FAST:     per_worker = SSBC7_ADAPT_SEED_MPIX_FAST;     break;
                    case SSBC7_QUALITY_BALANCED: per_worker = SSBC7_ADAPT_SEED_MPIX_BALANCED; break;
                    default:                     per_worker = SSBC7_ADAPT_SEED_MPIX_HIGH;     break;
                }
            }
            // CAPACITY, not achieved throughput: what the pool could drain if it were fed, which is the only sense in which "would this profile keep up" is a question with an answer. The pool being idle because nothing has arrived must never read as the pool being slow.
            in.mCapacityMpixPerSec[q] = per_worker * (F32)llmax(workers, 1u);
        }
    }

    in.mPinned        = !st->mAdaptive.load();
    in.mPinnedQuality = st->mPinned;

    const SSBC7Quality before = (SSBC7Quality)st->mQuality.load(std::memory_order_relaxed);
    const bool changed = st->mLadder.update(in);

    st->mLastCapacity = in.mCapacityMpixPerSec[(U32)st->mLadder.quality()];
    st->mQuality.store((U32)st->mLadder.quality(), std::memory_order_relaxed);

    if (changed)
    {
        // EVERY transition, at INFO, with the measurement and the backlog that caused it. A feature that silently changes quality is one nobody can debug, and this line is the whole of the audit trail: what it was, what it is, why, and the two numbers the decision was made from.
        LL_INFOS("Squeeze") << "BC7 encode quality " << ssBC7QualityName(before) << " -> " << ssBC7QualityName(st->mLadder.quality())
                            << ": " << ssBC7AdaptReasonName(st->mLadder.reason())
                            << ". Backlog " << backlog << " textures at about " << in.mMpixPerTexture
                            << " Mpix each, pool capacity " << in.mCapacityMpixPerSec[(U32)before]
                            << " Mpix/s over " << workers << " workers giving an estimated "
                            << ssBC7AdaptDrainSeconds(backlog, in.mMpixPerTexture, in.mCapacityMpixPerSec[(U32)before])
                            << " s to drain, against a step-down bar of " << SSBC7_ADAPT_DRAIN_SLOW_SECONDS
                            << " s and a step-up bar of " << SSBC7_ADAPT_DRAIN_FAST_SECONDS << " s" << LL_ENDL;

        if (st->mLadder.quality() == SSBC7_QUALITY_FAST)
        {
            // The bottom rung is worth saying out loud. ultrafast shares the portable backend's one blind spot - a 4x4 holding several unrelated hues scores about 11 dB - so records written here are genuinely poor, and the only reason it is on the ladder at all is that the idle upgrade pass will come back and redo them.
            LL_WARNS("Squeeze") << "BC7 encode quality is at its lowest rung: records written now will be visibly poor on blocks holding several unrelated colours, and are queued for re-encode by the idle upgrade pass once the machine catches up" << LL_ENDL;
        }
    }
}

// ---------------------------------------------------------------------------
// The readout
// ---------------------------------------------------------------------------

SSBC7AdaptiveStats ssBC7AdaptiveStatsNow()
{
    SSBC7AdaptiveStats out;

    AdaptiveState* st = state();
    if (!st) return out;

    const F64 now = nowSeconds();

    out.mQuality  = (U8)st->mLadder.quality();
    out.mReason   = (U8)st->mLadder.reason();
    out.mAdaptive = st->mAdaptive.load();
    out.mChanges  = st->mLadder.changes();

    // A single-encoder backend reads as "pinned" to the ladder, because that is exactly how it is driven - but saying "pinned by the user setting" to somebody who pinned nothing is a lie, so the readout names the real reason instead.
    if (!st->mBackendHasProfiles.load()) out.mReason = (U8)SSBC7_ADAPT_NO_BACKEND;

    out.mWorkers     = (U32)llmax(0, ssBC7EncodePoolWidth());
    out.mWorkersBusy = st->mBusyAverage;

    {
        std::lock_guard<std::mutex> lock(st->mWindowMutex);
        out.mBusySecondsTotal     = st->mWindow.busySeconds();
        out.mAggregateMpixPerSec  = st->mWindow.aggregateRate(now);
        for (U32 q = 0; q < (U32)SSBC7_QUALITY_COUNT; ++q)
        {
            out.mProfileMpixPerSec[q] = st->mWindow.rate((SSBC7Quality)q, now);
            out.mProfileSamples[q]    = st->mWindow.samples((SSBC7Quality)q, now);
        }
    }

    out.mCapacityMpixPerSec = st->mLastCapacity;
    out.mBacklog            = (U32)ssBC7EncodeWantListSize() + (U32)llmax(0, ssBC7EncodePendingCount());
    out.mDrainSeconds       = st->mLadder.drainSeconds();

    out.mUpgradeRunning = st->mUpgradeRunning.load() > 0;
    out.mUpgradeFailed  = st->mUpgradeFailed.load();

    if (SSBC7Store* store = SSBC7Store::getInstance())
    {
        // Both of these are counter reads rather than index walks, which is what makes it safe to call this every frame from an overlay.
        store->qualityHistogram(out.mRecordsAtQuality);
        out.mUpgraded = store->upgradesApplied();

        const U32 best = (U32)bestQuality();
        for (U32 q = 0; q < (U32)SSBC7_QUALITY_COUNT; ++q)
        {
            if (q != best) out.mBelowBest += out.mRecordsAtQuality[q];
        }
    }

    return out;
}

std::string ssBC7AdaptiveMetricsString()
{
    AdaptiveState* st = state();
    if (!st) return "BC7 adaptive quality not started";

    const SSBC7AdaptiveStats s = ssBC7AdaptiveStatsNow();

    return llformat("BC7 quality: %s (%s), %u changes | %.1f of %u workers busy, %.1f Mpix/s achieved, %.1f Mpix/s capacity | backlog %u textures, %.0f s to drain | upgraded %u, %u still below best, %u gave up",
                    ssBC7QualityName((SSBC7Quality)s.mQuality),
                    ssBC7AdaptReasonName((ESSBC7AdaptReason)s.mReason),
                    s.mChanges,
                    s.mWorkersBusy, s.mWorkers,
                    s.mAggregateMpixPerSec, s.mCapacityMpixPerSec,
                    s.mBacklog, s.mDrainSeconds,
                    s.mUpgraded, s.mBelowBest, s.mUpgradeFailed);
}
