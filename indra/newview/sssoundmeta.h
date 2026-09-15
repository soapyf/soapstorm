/**
 * @file sssoundmeta.h
 * @brief Atmo Magic: pre-analysed sound metadata.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef SS_SOUNDMETA_H
#define SS_SOUNDMETA_H

#include "llsingleton.h"
#include "lluuid.h"
#include "llassettype.h"
#include "llextendedstatus.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

class SSSoundMeta : public LLSingleton<SSSoundMeta>
{
    LLSINGLETON_EMPTY_CTOR(SSSoundMeta);

public:
    enum Purpose : U32
    {
        PURPOSE_TIMING  = 1,
        PURPOSE_LEVEL   = 2,
        PURPOSE_BRIGHT  = 4,
        PURPOSE_DENSITY = 8,
        PURPOSE_STEPS   = 16,
    };
    struct Meta
    {
        U32 mLengthMS = 0;
        U32 mOnsetMS = 0;
        U32 mTailMS = 0;
        F32 mPeakLevel = 0.f;
        F32 mImpactRate = 0.f;
        F32 mDensity = 0.f;
        F32 mCrackiness = 0.f;

        std::vector<U32> mOnsets;

        F32 mGapFloor = 1.f;

        F32 mCadenceCV = 9.f;
        U32 mRepaired = 0;

        U32 mPeakMS = 0;
        std::vector<F32> mEnvelope;
    };

    const Meta* get(const LLUUID& id);

    void idle();

    // <SS:Nexii> Fetches a sound from the asset server when the cache lacks it, then queues the decode. Stock preloadSound only decodes what is already on disk - the engine fetches solely for live audio sources - so Atmo's own ambience config would otherwise sit unready until something played it.
    void fetch(const LLUUID& id);

    S32 readyCount();
    S32 pendingCount();

private:
    void cleanupSingleton() override;

    void gather();
    void addList(const std::string& csv, const std::string& source, U32 purpose);
    void pump();
    void startWorkers();
    static void onAssetFetched(const LLUUID& id, LLAssetType::EType type, void* user_data, S32 status, LLExtStat ext_status);
    static Meta analyze(const std::vector<S16>& pcm, S32 channels, F32 rate, U32 purpose);

public:
    enum EState { EMPTY, PENDING, ANALYZING, READY, FAILED };
private:
    struct Entry
    {
        EState mState = PENDING;
        F64 mFirstTried = -1.0;
        bool mFetchIssued = false;
        std::string mSource;
        U32 mPurpose = 0;
        Meta mMeta;
        std::string mFailWhy;
    };

public:
    const std::map<LLUUID, Entry>& entriesForDebug() const { return mEntries; }

    // <SS:Nexii> One record per configured slot from the last gather, in definition order,
    // so the debug view can keep sequence order visible and show slots that name no sound
    // at all - those produce no entry of their own.
    struct SlotInfo
    {
        std::string mSource;
        U32 mPurpose = 0;
        std::vector<LLUUID> mSounds;    // the slot's sounds in definition (sequence) order
    };
    const std::vector<SlotInfo>& slotsForDebug() const { return mSlots; }

private:

    struct Job
    {
        LLUUID mID;
        std::vector<S16> mPCM;
        S32 mChannels = 1;
        F32 mRate = 44100.f;
        U32 mLengthMS = 0;
        U32 mPurpose = 0;
    };

    std::map<LLUUID, Entry> mEntries;
    std::vector<SlotInfo> mSlots;
    std::set<LLUUID> mFetching;
    F64 mLastGather = -1.0;

    std::vector<std::thread> mWorkers;
    std::deque<Job> mJobs;
    std::vector<std::pair<LLUUID, Meta>> mResults;
    std::mutex mJobMutex;
    std::mutex mResultMutex;
    std::condition_variable mJobSignal;
    bool mStop = false;
    bool mStarted = false;
};

#endif
