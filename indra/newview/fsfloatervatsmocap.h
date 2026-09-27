/**
 * @file fsfloatervatsmocap.h
 * @brief Viewport Avatar Toolset motion capture in the viewer: VMC, Rokoko Studio Live and iFacialMocap (face
 *        and eyes) driving your own avatar live, recorded into the VATs Animator's clip (VATs spec 09,
 *        stage 6h). The same window as the VATs app's Tools > Motion Capture.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Copyright (c) 2026 Viewport Avatar Toolset contributors
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

#ifndef FS_FLOATERVATSMOCAP_H
#define FS_FLOATERVATSMOCAP_H

#include "llfloater.h"

#include <future>
#include <memory>

#include "fsvatsfirewall.h"
#include "fsvatsudp.h"
#include "vats/mocap.h"
#include "vats/rig.h"
#include "vats/skeleton.h"

class FSFloaterAnimator;

class FSFloaterVATsMocap : public LLFloater
{
public:
    FSFloaterVATsMocap(const LLSD& key);
    ~FSFloaterVATsMocap() override;

    bool postBuild() override;
    void onClose(bool app_quitting) override;
    void draw() override;

private:
    static void onIdle(void* self);
    void poll();                 // every frame while listening, recording or waiting on the firewall
    void updateIdle();           // registers the idle callback only while there is something to poll
    void setListening(bool on);
    bool loadData();             // the skeleton and the retarget and face tables, once
    void driveLive();            // the live pose onto the avatar (VATsClipMotion::sLive)
    void stopLive();
    void startRecording();
    void commitTake();
    void cancelTake(const std::string& why);
    void applyPreset(const std::string& name);
    void refresh();
    void setStatus(const std::string& text);
    FSFloaterAnimator* animator() const;

    // Connection
    vats::UdpReceiver mSock;
    int mSource = 0;     // 0 VMC, 1 Rokoko Studio Live (JSON v3), 2 iFacialMocap (face only)
    int mPort = 39539;   // VMC's usual port
    bool mLan[3] = { false, false, true };  // accept senders on other devices, per source; a phone always is one
    std::string mActor;  // Rokoko: the actor streamed
    std::string mError;
    int mCount = 0, mPps = 0;
    double mWindowStart = 0, mLastPacket = -1, mListenStart = 0;
    std::string mSender;
    bool mRemoteSeen = false;  // a packet came from another device, so nothing is blocking

    // Setup checklist
    std::vector<vats::LanAddress> mAddrs;
    double mAddrsAt = -100;
    vats::Firewall mFirewall = vats::Firewall::Unknown;
    std::future<vats::Firewall> mDetectJob;
    bool mDetected = false;
    std::future<int> mAllowJob;
    int mAllowedPort = 0;
    std::string mAllowMessage;
    bool mAllowFailed = false;

    // Mapping
    vats::Skeleton mSkel;
    std::unique_ptr<vats::Rig> mRig;
    vats::RigTable mTable;
    vats::FaceTable mFace;
    std::string mDataError;
    vats::FaceSettings mFaceSettings;
    bool mFaceOn = true, mFaceOnly = false;
    vats::VmcState mState, mRest;
    bool mHaveData = false, mRestFromPose = false, mDrive = true;

    // Live drive
    vats::Clip mLive;
    LLUUID mLiveID;
    std::vector<std::string> mLiveJoints;

    // Recording
    vats::MocapRecorder mRec;
    unsigned mTakeGeneration = 0;  // the Animator clip the take belongs to
    int mFrom = 0, mTo = 30;
    bool mPunchOut = false, mSelectedOnly = false;
    float mCountdown = 3;
    std::vector<std::string> mOnly;
    vats::MocapCleanup mClean;
    std::vector<std::string> mReport;

    bool mIdle = false;
};

#endif // FS_FLOATERVATSMOCAP_H
