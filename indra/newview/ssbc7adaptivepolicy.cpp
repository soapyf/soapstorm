/**
 * @file ssbc7adaptivepolicy.cpp
 * @brief Squeeze adaptive encode quality - the moving window and the control law, with no viewer attached, see doc/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssbc7adaptive.h"

#include <cstring>

// Everything in this file is deliberately free of gSavedSettings, the thread pool, the store and even a clock, so the offline harness compiles and runs THIS source rather than a re-implementation of it. The two things worth getting wrong here - whether the window can be talked into reporting a rate it never measured, and whether the ladder can be made to oscillate - are both decided in this file and nowhere else.

// ---------------------------------------------------------------------------
// Reason names
// ---------------------------------------------------------------------------

namespace
{
    const char* s_adapt_reason_names[SSBC7_ADAPT_REASON_COUNT] =
    {
        "pinned by the user setting",
        "no encodes measured yet",
        "best quality, keeping up easily",
        "holding, no sustained signal",
        "stepped down under backlog",
        "stepped back up as the backlog cleared",
        "this block backend has only one encoder"
    };

    // The rungs, BEST FIRST. Ordered by measured cost rather than by the profile names, which is the whole reason this array exists instead of a loop over the enum: it was built for bc7e, whose `slow` was faster than its `basic`, and it stays because the ordinal ordering of SSBC7Quality is a fact about the on-disk quality byte, not about whichever backend is linked - this array is what pins the ladder down if a fourth profile is ever added in the middle. With bc7f the three presets happen to be monotonic in both cost and quality.
    const SSBC7Quality s_rungs[SSBC7_QUALITY_COUNT] =
    {
        SSBC7_QUALITY_HIGH,
        SSBC7_QUALITY_BALANCED,
        SSBC7_QUALITY_FAST
    };

    U32 rungOf(SSBC7Quality quality)
    {
        for (U32 i = 0; i < (U32)SSBC7_QUALITY_COUNT; ++i)
        {
            if (s_rungs[i] == quality) return i;
        }
        return 0;   // an unknown profile is treated as the best rung, so a bad value degrades to "encode well" rather than to "encode badly"
    }
}

const char* ssBC7AdaptReasonName(ESSBC7AdaptReason reason)
{
    if ((U32)reason >= (U32)SSBC7_ADAPT_REASON_COUNT) return "unknown";
    return s_adapt_reason_names[reason];
}

// ---------------------------------------------------------------------------
// The moving window
// ---------------------------------------------------------------------------

SSBC7RateWindow::SSBC7RateWindow()
:   mBusySeconds(0.0),
    mEncodes(0)
{
    memset(mRing, 0, sizeof(mRing));
    for (U32 q = 0; q < (U32)SSBC7_QUALITY_COUNT; ++q)
    {
        mFilled[q] = 0;
        mNext[q]   = 0;
    }
}

void SSBC7RateWindow::add(SSBC7Quality quality, F32 mpix, F32 seconds, F64 now)
{
    const U32 q = (U32)quality;
    if (q >= (U32)SSBC7_QUALITY_COUNT) return;

    // A zero or negative duration is a clock that went backwards or a timer that was never started, and admitting it would divide by nothing and report an infinite rate that then justifies the best profile forever. Dropped rather than clamped, because a sample with no duration carries no information at all.
    if (!(mpix > 0.f) || !(seconds > 0.f)) return;

    Sample& slot = mRing[q][mNext[q]];
    slot.mWhen    = now;
    slot.mMpix    = mpix;
    slot.mSeconds = seconds;

    mNext[q] = (mNext[q] + 1) % SSBC7_ADAPT_WINDOW_SAMPLES;
    if (mFilled[q] < SSBC7_ADAPT_WINDOW_SAMPLES) ++mFilled[q];

    mBusySeconds += (F64)seconds;
    ++mEncodes;
}

F32 SSBC7RateWindow::rate(SSBC7Quality quality, F64 now) const
{
    const U32 q = (U32)quality;
    if (q >= (U32)SSBC7_QUALITY_COUNT || mFilled[q] == 0) return 0.f;

    const F64 horizon = now - (F64)SSBC7_ADAPT_WINDOW_SECONDS;

    F64 mpix_in = 0.0, secs_in = 0.0;
    F64 mpix_all = 0.0, secs_all = 0.0;
    U32 count_in = 0;

    for (U32 i = 0; i < mFilled[q]; ++i)
    {
        const Sample& s = mRing[q][i];
        mpix_all += s.mMpix;
        secs_all += s.mSeconds;
        if (s.mWhen >= horizon)
        {
            mpix_in += s.mMpix;
            secs_in += s.mSeconds;
            ++count_in;
        }
    }

    // THE FALLBACK IS THE POINT. This pool is supply limited, so a perfectly healthy machine can go a whole minute without encoding anything, and a window that reported nothing in that case would send the ladder back to its seeded figures every time the user stood still. Old samples are used instead: a stale measurement of THIS machine beats a fresh benchmark figure from a different one.
    if (count_in >= SSBC7_ADAPT_MIN_SAMPLES && secs_in > 0.0) return (F32)(mpix_in / secs_in);
    if (secs_all > 0.0)                                       return (F32)(mpix_all / secs_all);
    return 0.f;
}

U32 SSBC7RateWindow::samples(SSBC7Quality quality, F64 now) const
{
    const U32 q = (U32)quality;
    if (q >= (U32)SSBC7_QUALITY_COUNT) return 0;

    const F64 horizon = now - (F64)SSBC7_ADAPT_WINDOW_SECONDS;
    U32 count_in = 0;
    for (U32 i = 0; i < mFilled[q]; ++i)
    {
        if (mRing[q][i].mWhen >= horizon) ++count_in;
    }
    // Reports whichever set the rate above would actually have used, so a readout never says "two samples" beside a figure built from forty.
    return (count_in >= SSBC7_ADAPT_MIN_SAMPLES) ? count_in : mFilled[q];
}

F32 SSBC7RateWindow::aggregateRate(F64 now) const
{
    const F64 horizon = now - (F64)SSBC7_ADAPT_WINDOW_SECONDS;

    F64 mpix = 0.0;
    F64 oldest = now;
    U32 count = 0;

    for (U32 q = 0; q < (U32)SSBC7_QUALITY_COUNT; ++q)
    {
        for (U32 i = 0; i < mFilled[q]; ++i)
        {
            const Sample& s = mRing[q][i];
            if (s.mWhen < horizon) continue;
            mpix += s.mMpix;
            if (s.mWhen < oldest) oldest = s.mWhen;
            ++count;
        }
    }

    if (count == 0) return 0.f;

    // Divided by WALL TIME rather than by the sum of the workers' busy seconds, which is what makes this the achieved throughput of the whole machine rather than the capability of one thread. Idle workers pull it down, and that is correct: it is the answer to "how fast is my cache filling", not "how fast could it".
    //
    // The span is the shorter of the window and the time since the oldest sample in it, so the first few seconds of a session do not read as a near-zero rate simply because the window has not filled yet.
    F64 span = now - oldest;
    if (span < 0.25) span = 0.25;   // a burst of encodes inside a quarter second would otherwise divide by almost nothing and report a rate no machine has
    return (F32)(mpix / span);
}

F32 SSBC7RateWindow::meanMpixPerTexture(F64 now) const
{
    const F64 horizon = now - (F64)SSBC7_ADAPT_WINDOW_SECONDS;

    F64 mpix_in = 0.0, mpix_all = 0.0;
    U32 count_in = 0, count_all = 0;

    for (U32 q = 0; q < (U32)SSBC7_QUALITY_COUNT; ++q)
    {
        for (U32 i = 0; i < mFilled[q]; ++i)
        {
            const Sample& s = mRing[q][i];
            mpix_all += s.mMpix;
            ++count_all;
            if (s.mWhen >= horizon) { mpix_in += s.mMpix; ++count_in; }
        }
    }

    if (count_in >= SSBC7_ADAPT_MIN_SAMPLES) return (F32)(mpix_in / (F64)count_in);
    if (count_all > 0)                       return (F32)(mpix_all / (F64)count_all);
    return SSBC7_ADAPT_SEED_MPIX_PER_TEXTURE;
}

// ---------------------------------------------------------------------------
// The control law
// ---------------------------------------------------------------------------

F32 ssBC7AdaptDrainSeconds(U32 backlog, F32 mpix_per_texture, F32 capacity_mpix_per_sec)
{
    if (backlog == 0) return 0.f;
    if (!(capacity_mpix_per_sec > 0.f) || !(mpix_per_texture > 0.f)) return -1.f;   // unknown, and every caller has to treat that as "hold" rather than as "instant"

    return ((F32)backlog * mpix_per_texture) / capacity_mpix_per_sec;
}

SSBC7QualityLadder::SSBC7QualityLadder()
:   mQuality(SSBC7_QUALITY_HIGH),
    mReason(SSBC7_ADAPT_NO_DATA),
    mDrainSeconds(0.f),
    mBehindSince(0.0),
    mComfortableSince(0.0),
    mLastChange(0.0),
    mChanges(0)
{
}

void SSBC7QualityLadder::reset(SSBC7Quality quality, F64 now)
{
    mQuality          = quality;
    mReason           = SSBC7_ADAPT_NO_DATA;
    mDrainSeconds     = 0.f;
    mBehindSince      = 0.0;
    mComfortableSince = 0.0;
    // The cooldown starts armed rather than expired, so a settings change followed immediately by a busy minute cannot produce two profile changes inside the first few seconds of a session.
    mLastChange       = now;
    // mChanges is NOT cleared: it counts what the session did, and a readout that reset it on every settings change would understate how much the controller has been moving.
}

F32 SSBC7QualityLadder::behindFor(F64 now) const
{
    return (mBehindSince > 0.0) ? (F32)(now - mBehindSince) : 0.f;
}

F32 SSBC7QualityLadder::comfortableFor(F64 now) const
{
    return (mComfortableSince > 0.0) ? (F32)(now - mComfortableSince) : 0.f;
}

bool SSBC7QualityLadder::update(const SSBC7LadderInput& in)
{
    // THE USER'S SETTING ALWAYS WINS, and it wins before anything else is even computed. Nothing below this can run, so there is no path by which a measurement talks the controller into overriding a pinned profile - which is the one behaviour a person who deliberately chose HIGH would never forgive.
    if (in.mPinned)
    {
        const bool changed = (mQuality != in.mPinnedQuality);
        mQuality          = in.mPinnedQuality;
        mReason           = SSBC7_ADAPT_PINNED;
        mDrainSeconds     = ssBC7AdaptDrainSeconds(in.mBacklog, in.mMpixPerTexture, in.mCapacityMpixPerSec[(U32)mQuality]);
        mBehindSince      = 0.0;
        mComfortableSince = 0.0;
        if (changed) { mLastChange = in.mNow; ++mChanges; }
        return changed;
    }

    const U32 rung = rungOf(mQuality);

    const F32 here = ssBC7AdaptDrainSeconds(in.mBacklog, in.mMpixPerTexture, in.mCapacityMpixPerSec[(U32)mQuality]);
    mDrainSeconds  = here;

    // An unknown drain is not a signal in either direction. It happens when nothing has been encoded at the current profile yet, and acting on it would mean stepping down on the strength of having no evidence.
    const bool behind = (here >= 0.f) && (here > SSBC7_ADAPT_DRAIN_SLOW_SECONDS);

    // THE STEP UP IS TESTED AGAINST THE TARGET, NOT AGAINST HERE, and that is what makes the ladder stable rather than merely slow. Asking "is the current profile comfortable" would step up whenever a cheap profile had caught up - and the expensive profile it stepped up to could then be eight times slower and immediately fall behind again. Asking "would the BETTER profile still be comfortable" makes the two tests apply to the same quantity with a dead band between them, so a step up cannot undo itself.
    const bool at_best   = (rung == 0);
    const F32  there     = at_best ? here : ssBC7AdaptDrainSeconds(in.mBacklog, in.mMpixPerTexture, in.mCapacityMpixPerSec[(U32)s_rungs[rung - 1]]);
    const bool comfortable = (there >= 0.f) && (there < SSBC7_ADAPT_DRAIN_FAST_SECONDS);

    // Each direction remembers when its condition last became true, and loses that the instant it stops being true. A noisy signal therefore never accumulates: it has to hold continuously for the whole dwell, so one tick of relief is enough to cancel a step down and one tick of backlog is enough to cancel a step up.
    if (behind) { if (mBehindSince == 0.0) mBehindSince = in.mNow; }
    else        { mBehindSince = 0.0; }

    if (comfortable) { if (mComfortableSince == 0.0) mComfortableSince = in.mNow; }
    else             { mComfortableSince = 0.0; }

    const bool cooled = (in.mNow - mLastChange) >= (F64)SSBC7_ADAPT_COOLDOWN_SECONDS;

    if (cooled && behind && rung + 1 < (U32)SSBC7_QUALITY_COUNT
        && (in.mNow - mBehindSince) >= (F64)SSBC7_ADAPT_DOWN_DWELL_SECONDS)
    {
        mQuality          = s_rungs[rung + 1];
        mReason           = SSBC7_ADAPT_STEPPED_DOWN;
        mLastChange       = in.mNow;
        // Both timers are cleared, not just the one that fired: the signal that justified this change has been acted on, and letting the other one carry credit accumulated before the change would let two moves happen back to back on one observation.
        mBehindSince      = 0.0;
        mComfortableSince = 0.0;
        ++mChanges;
        return true;
    }

    if (cooled && comfortable && rung > 0
        && (in.mNow - mComfortableSince) >= (F64)SSBC7_ADAPT_UP_DWELL_SECONDS)
    {
        mQuality          = s_rungs[rung - 1];
        mReason           = SSBC7_ADAPT_STEPPED_UP;
        mLastChange       = in.mNow;
        mBehindSince      = 0.0;
        mComfortableSince = 0.0;
        ++mChanges;
        return true;
    }

    // A TRANSITION REASON IS STICKY, and it has to be, because the overlay reads this between ticks rather than at the instant of the change: overwriting "stepped down under backlog" with "holding" on the very next tick would mean a user could never see WHY the profile is where it is, only that it is there. It survives until the next transition or until the ladder is back at its best rung with room to spare, at which point "best quality, keeping up easily" is the more useful thing to say.
    const bool was_transition = (mReason == SSBC7_ADAPT_STEPPED_DOWN || mReason == SSBC7_ADAPT_STEPPED_UP);

    if (here < 0.f)                     mReason = SSBC7_ADAPT_NO_DATA;
    else if (at_best && comfortable)    mReason = SSBC7_ADAPT_HEADROOM;
    else if (was_transition)            { /* left exactly as it was */ }
    else                                mReason = SSBC7_ADAPT_HOLDING;

    return false;
}
