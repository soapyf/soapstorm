/**
 * @file sssoundscape.h
 * @brief Atmo Magic: the weather soundscape.
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

#ifndef SS_SOUNDSCAPE_H
#define SS_SOUNDSCAPE_H

#include "llsingleton.h"
#include "lluuid.h"
#include "ssworldfield.h"
#include "v3math.h"

#include <map>
#include <string>
#include <vector>

class LLAudioSource;
class LLHUDText;

class SSSoundscape : public LLSingleton<SSSoundscape>
{
    LLSINGLETON_EMPTY_CTOR(SSSoundscape);

public:
    void idle();

    void notifyImpact(F32 strength);

    void stopAll();

    LLUUID footstepSound(const LLUUID& avatar_id, const LLVector3& foot_pos_agent,
                         bool on_land, S32 action, bool is_self);

    void updateFootstepLoop(const LLUUID& avatar_id, const LLVector3& pos_agent,
                            bool on_land, S32 locomotion, bool is_self);

    void footstepEvent(const LLUUID& avatar_id, const LLVector3& pos_agent,
                       bool on_land, S32 action, bool is_self);

    void footstepImpact(const LLUUID& avatar_id, const LLVector3& foot_pos_agent, bool is_self);

    struct StepDebug
    {
        F64 mWhen = -1.0;
        S32 mSurface = -1;
        S32 mAction = -1;
        bool mIndoors = false;
        char mIndoorsFrom = '-';
        char mMode = '-';
        bool mFieldValid = false;
        F32 mWet = 0.f;
        F32 mPuddle = 0.f;
        bool mGlobal = false;
        std::string mSource;
        S32 mListSize = 0;
        LLUUID mPicked;
        const char* mWhyNot = "";
        F32 mStepGap = 0.f;     // seconds between the last two footfalls that actually played - compare against the gait to see whether steps are being missed
        S32 mStepDropped = 0;   // footfalls refused by the anti-spam gap since this avatar started moving
    };
    const StepDebug& lastStep(bool self) const { return self ? mStepSelf : mStepOther; }

    void scheduleThunder(const LLVector3& pos_agent, F32 distance_m, F32 intensity,
                         F64 fire_at, F32 muffle = 0.f);

    void playCharge(const LLVector3& pos_agent, F32 intensity);

    F32 windCarryGain(const LLVector3& source_pos_agent) const;

    F32 skyOcclusion() const;

    S32 pendingThunder() const { return (S32)mThunder.size(); }

    enum ESpace
    {
        SPACE_OUTDOOR = 0,
        SPACE_SHELTERED,
        SPACE_SMALL,
        SPACE_MEDIUM,
        SPACE_BIG
    };

    enum ESize
    {
        SIZE_SMALL = 0,
        SIZE_MEDIUM,
        SIZE_LARGE
    };
    bool isCovered() const { return mCovered; }
    F32 coverage() const { return mCoverage; }
    // <SS:Nexii> Whether the air flood considers the listener's cell sealed - interior air the connectivity walk could not reach from sky or border. Reads true only on the field coverage path.
    bool isInterior() const { return mInterior; }
    ESpace space() const { return mSpace; }

    static const char* spaceName(ESpace space);
    static const char* sizeName(ESize size);
    ESize outdoorSize() const { return mOutdoorSize; }

    F32 occlusionGain(const LLVector3& source_pos) const;
    F32 wallDistanceToward(const LLVector3& dir_horizontal) const;

    F32 impactRate() const;
    F32 coverBlend() const { return mCoverSmooth; }
    S32 wallCount() const { return mWallCount; }
    F32 wallDistance() const { return mWallAvg; }
    F32 roofDistance() const { return mRoofDist; }

    F32 burialDepth() const { return mBuriedSmooth; }

    F32 burialOcclusion() const;

    S32 activeLoops() const;
    F64 lastProbeAge() const;

private:
    StepDebug mStepSelf;
    StepDebug mStepOther;

    struct PendingThunder
    {
        F64 mHeardAt = 0.0;
        F64 mPlayAt = 0.0;
        LLVector3 mPos;
        F32 mGain = 1.f;
        F32 mDistanceM = 0.f;
        F32 mMuffle = 0.f;
        LLUUID mSound;
        bool mAligned = false;
    };
    std::vector<PendingThunder> mThunder;

    void queueThunder(const LLUUID& sound, const LLVector3& pos_agent,
                      F32 distance_m, F32 gain, F64 heard_at, F32 muffle);

    struct AvatarCover
    {
        bool mIndoors = false;
        LLVector3 mPos;
        F64 mWhen = -1.0;

        bool mOnObject = false;
        LLVector3 mFootPos;
        F64 mFootWhen = -1.0;
    };
    std::map<LLUUID, AvatarCover> mAvatarCover;
    bool roofOver(const LLUUID& avatar_id, const LLVector3& pos_agent, bool is_self);

    bool onObject(const LLUUID& avatar_id, const LLVector3& foot_pos_agent, bool is_self);

    struct StepLoop
    {
        LLUUID mSourceID;
        LLUUID mSound;
        S32 mSurface = -1;
        S32 mAction = -1;
        F64 mLastSeen = 0.0;
        F64 mStartedAt = 0.0;
        U32 mOffsetMS = 0;
        bool mSegmented = false;
        F64 mLastImpactAt = 0.0;
        LLUUID mSegSourceID;
        F64 mStopAt = 0.0;
    };
    std::map<LLUUID, StepLoop> mStepLoops;
    void releaseStepLoop(StepLoop& loop);
    void reapStepLoops(F64 now);

    struct OneShotFollower
    {
        LLUUID mSourceID;
        LLVector3 mDir;
        LLVector3 mLastPos;
        F64 mExpires = 0.0;
    };
    std::vector<OneShotFollower> mFollowers;

    F32 mWetSlow = 0.f;

    struct BedVoice
    {
        LLUUID mSourceID;
        F64 mStartedAt = 0.0;
        U32 mOffsetMS = 0;
        F32 mGain = 0.f;
        F32 mTarget = 0.f;
    };
    std::map<LLUUID, BedVoice> mBedVoices;
    std::map<LLUUID, U32> mBedResume;

    struct StepMark
    {
        LLUUID mSourceID;
        LLHUDText* mText = nullptr;
        F64 mStart = 0.0;
    };
    std::vector<StepMark> mStepMarks;
    void markStepSource(const LLUUID& source_id);
    void updateStepMarks(F64 now);

    std::vector<std::pair<LLUUID, F64>> mSegmentStops;
    void updateSegmentStops(F64 now);

    std::vector<std::pair<LLUUID, F64>> mDying;
    void fadeKill(const LLUUID& source_id);
    void updateDying(F64 now);
    std::vector<std::pair<LLUUID, F32>> mLadderTargets;
    void updateBedVoices(F64 now, F32 dt, F32 master_mul);
    void registerFollower(const LLUUID& source_id, const LLVector3& dir_world, F64 now);
    void updateFollowers(F64 now, F32 dt);
    void updateThunder(F64 now);

    enum ESlot
    {
        LOOP_AMBIENT_LIGHT = 0,
        LOOP_AMBIENT_MEDIUM,
        LOOP_AMBIENT_HEAVY,
        LOOP_ROOF_OPEN,
        LOOP_ROOF_SMALL,
        LOOP_ROOF_MEDIUM,
        LOOP_ROOF_BIG,
        LOOP_WIND_LIGHT,
        LOOP_WIND_STRONG,
        LOOP_COUNT
    };

    struct Loop
    {
        std::vector<LLUUID> mSounds;
        std::string mConfigured;
        U32 mIndex = 0;
        LLUUID mSourceID;
        F32 mGain = 0.f;
        F32 mTarget = 0.f;

        LLVector3 mOffset;
    };

    void updateProbes(F64 now);
    bool castUpProbe(S32 index, F32& hit_dist);
    F32 castSideProbe(S32 index);
    void updateLoops(F64 now, F32 dt);
    void applyLoop(Loop& loop, const std::string& configured, F32 master, F32 dt);
    void releaseLoop(Loop& loop);

    Loop mLoops[LOOP_COUNT];

    // <SS:Nexii> The COVERAGE stake in the shared world field, held for the camera's region while SSWorldFieldCoverage routes the cover and burial questions through the field's band stack instead of the up-raycasts. Holding the handle is what makes the field build tiles here at all.
    SSWorldField::Interest mCoverageClaim;
    U64 mCoverageRegion = 0;

    LLVector3 mProbeAnchor;
    F64 mLastCycleDone = -1000.0;
    F32 mSideDist[4] = { 0.f, 0.f, 0.f, 0.f };
    F32 mRoofDist = 0.f;
    S32 mWallCount = 0;
    F32 mWallAvg = 0.f;
    F32 mCoverage = 0.f;
    bool mCovered = false;
    // <SS:Nexii> Sealed-room verdict from the world field's air flood, set on the field coverage path and cleared everywhere else.
    bool mInterior = false;
    ESpace mSpace = SPACE_OUTDOOR;
    ESize mOutdoorSize = SIZE_LARGE;
    LLVector3 mProbeOrigin;
    F32 mCoverSmooth = 0.f;

    F32 mBuriedDepth = 0.f;
    F32 mBuriedSmooth = 0.f;

    F32 mImpactRate = 0.f;
    F64 mLastIdle = 0.0;
};

#endif
