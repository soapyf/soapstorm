/**
 * @file fsfloatervatsmocap.cpp
 * @brief Viewport Avatar Toolset motion capture in the viewer (VATs spec 09, stage 6h). Parsing, mapping and
 *        writing keys are VATs' core (vats/mocap.h, vats/facecap.h); this owns the socket, the window,
 *        the live drive and handing a take to the VATs Animator.
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

#include "llviewerprecompiledheaders.h"

#include "fsfloatervatsmocap.h"

#include "fsfloateranimator.h"
#include "fsvatsclipmotion.h"
#include "llagent.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llclipboard.h"
#include "llcombobox.h"
#include "lldir.h"
#include "llfloaterreg.h"
#include "lltextbox.h"
#include "lltimer.h"
#include "llvoavatarself.h"

#include "vats/pose_ops.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>



namespace
{
    double now_s() { return F64(LLTimer::getTotalSeconds()); }

    template <class T> bool ready(const std::future<T>& f)
    {
        return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

    std::string read_file(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    const LLColor4 kDone(0.43f, 0.82f, 0.47f, 1.f), kAttention(0.9f, 0.67f, 0.27f, 1.f), kNotYet(0.6f, 0.6f, 0.6f, 1.f);
}

FSFloaterVATsMocap::FSFloaterVATsMocap(const LLSD& key) : LLFloater(key) {}

FSFloaterVATsMocap::~FSFloaterVATsMocap()
{
    stopLive();
    if (mIdle)
        gIdleCallbacks.deleteFunction(onIdle, this);
    // A pending firewall job finishes on its own thread; the futures wait for it here.
}

bool FSFloaterVATsMocap::postBuild()
{
    auto on = [this](const char* name, std::function<void(const LLSD&)> f)
    { getChild<LLUICtrl>(name)->setCommitCallback([f](LLUICtrl*, const LLSD& v) { f(v); }); };
    auto value = [this](const char* name) { return getChild<LLUICtrl>(name)->getValue(); };

    // Connection
    on("source_combo", [this, value](const LLSD&)
    {
        const int previous = mSource;
        mSource = std::clamp(int(value("source_combo").asInteger()), 0, 2);
        const int defaults[3] = { 39539, vats::kRokokoPort, vats::kIFacialMocapPort };
        if (mPort == defaults[previous])  // keep a port the user chose
            mPort = defaults[mSource];
        mRestFromPose = false;
        refresh();
    });
    on("port_spin", [this](const LLSD& v) { mPort = std::clamp(int(v.asInteger()), 1, 65535); });
    on("lan_check", [this](const LLSD& v) { mLan[mSource] = v.asBoolean(); });
    on("listen_btn", [this](const LLSD&) { setListening(!mSock.is_open()); });
    on("lan_fix_btn", [this](const LLSD&)
    {
        mLan[mSource] = true;
        setListening(false);
        setListening(true);
    });
    on("copy_addr_btn", [this](const LLSD&)
    {
        if (mAddrs.empty())
            return;
        const LLWString text = utf8str_to_wstring(mAddrs.front().ip);
        LLClipboard::instance().copyToClipboard(text, 0, S32(text.size()));
        setStatus("Copied " + mAddrs.front().ip);
    });
    on("fw_allow_btn", [this](const LLSD&)
    {
        if (mAddrs.empty() || mAllowJob.valid())
            return;
        // ponytail: first network only, as the app; add a picker if people have two.
        const std::string cmd = vats::allow_command(mFirewall, mPort, mAddrs.front().subnet, "pkexec");
        mAllowMessage.clear();
        mAllowJob = std::async(std::launch::async, [cmd] { std::string out; return vats::system_runner()(cmd, out); });
        updateIdle();
    });
    on("copy_cmd_btn", [this](const LLSD&)
    {
        if (mAddrs.empty())
            return;
        const LLWString text = utf8str_to_wstring(vats::allow_command(mFirewall, mPort, mAddrs.front().subnet, "sudo"));
        LLClipboard::instance().copyToClipboard(text, 0, S32(text.size()));
        setStatus("Copied the firewall command");
    });
    on("connect_btn", [this, value](const LLSD&)
    {
        const std::string ip = value("phone_edit").asString();
        mError.clear();
        if (mSock.send_to(ip, vats::kIFacialMocapPort, vats::kIFacialMocapHello, mError))
            setStatus("Asked the iPhone to start streaming");
    });
    // Live
    on("drive_check", [this](const LLSD& v) { mDrive = v.asBoolean(); if (!mDrive) stopLive(); });
    on("rest_capture_btn", [this](const LLSD&)
    {
        mRest = mState;
        mRest.root.rot = vats::Quat{};
        mRestFromPose = true;
    });
    on("rest_reset_btn", [this](const LLSD&)
    {
        mRest = vats::vmc_t_pose(mState);
        mRestFromPose = false;
    });
    // Face
    on("face_check", [this](const LLSD& v) { mFaceOn = v.asBoolean(); if (!mFaceOn) mFaceOnly = false; refresh(); });
    on("preset_combo", [this](const LLSD&) { applyPreset(getChild<LLComboBox>("preset_combo")->getSimple()); });
    on("strength_slider", [this](const LLSD& v) { mFaceSettings.gain = v.asReal(); });
    on("eye_slider", [this](const LLSD& v) { mFaceSettings.eye_gain = v.asReal(); });
    on("eye_yaw_slider", [this](const LLSD& v) { mFaceSettings.eye_yaw_max = v.asReal(); });
    on("eye_pitch_slider", [this](const LLSD& v) { mFaceSettings.eye_pitch_max = v.asReal(); });
    on("shape_combo", [this](const LLSD&)
    {
        const std::string shape = getChild<LLComboBox>("shape_combo")->getSimple();
        auto it = mFaceSettings.gains.find(shape);
        getChild<LLUICtrl>("shape_slider")->setValue(it != mFaceSettings.gains.end() ? it->second : 1.0);
    });
    on("shape_slider", [this](const LLSD& v)
    {
        const std::string shape = getChild<LLComboBox>("shape_combo")->getSimple();
        if (!shape.empty())
            mFaceSettings.gains[shape] = v.asReal();
    });
    on("neutral_btn", [this](const LLSD&)
    {
        mFaceSettings.neutral.clear();
        for (const auto& [name, w] : vats::face_weights(mFace, mState.blend, vats::FaceSettings{}))
            mFaceSettings.neutral[name] = w;
        setStatus("Captured your neutral face");
    });
    on("neutral_clear_btn", [this](const LLSD&) { mFaceSettings.neutral.clear(); });
    on("head_check", [this](const LLSD& v) { mFaceSettings.head = v.asBoolean(); });
    // Record
    on("from_spin", [this](const LLSD& v) { mFrom = std::max(int(v.asInteger()), 0); });
    on("current_btn", [this](const LLSD&)
    {
        if (FSFloaterAnimator* a = animator())
            getChild<LLUICtrl>("from_spin")->setValue(mFrom = a->currentFrame());
    });
    on("punch_check", [this](const LLSD& v) { mPunchOut = v.asBoolean(); refresh(); });
    on("to_spin", [this](const LLSD& v) { mTo = std::max(int(v.asInteger()), 0); });
    on("countdown_slider", [this](const LLSD& v) { mCountdown = F32(v.asReal()); });
    on("selected_check", [this](const LLSD& v) { mSelectedOnly = v.asBoolean(); if (mSelectedOnly) mFaceOnly = false; refresh(); });
    on("face_only_check", [this](const LLSD& v) { mFaceOnly = v.asBoolean(); if (mFaceOnly) mSelectedOnly = false; refresh(); });
    on("record_btn", [this](const LLSD&)
    {
        if (!mRec.active())
            startRecording();
        else if (mRec.counting_down(now_s()))
            cancelTake("Cancelled");
        else
        {
            mRec.stop();
            commitTake();
        }
    });
    on("smooth_slider", [this](const LLSD& v) { mClean.smooth = int(v.asInteger()); });
    on("reduce_check", [this](const LLSD& v) { mClean.reduce = v.asBoolean(); refresh(); });
    on("deg_spin", [this](const LLSD& v) { mClean.rot_deg = v.asReal(); });
    on("mm_spin", [this](const LLSD& v) { mClean.pos_m = v.asReal() / 1000; });
    on("blend_slider", [this](const LLSD& v) { mClean.blend = int(v.asInteger()); });
    on("feet_check", [this](const LLSD& v) { mClean.lock_feet = v.asBoolean(); });

    getChild<LLUICtrl>("source_combo")->setValue(mSource);
    getChild<LLUICtrl>("port_spin")->setValue(mPort);
    getChild<LLUICtrl>("to_spin")->setValue(mTo);
    getChild<LLUICtrl>("feet_check")->setValue(mClean.lock_feet);
    loadData();
    updateIdle();  // the firewall check starts at once, as in the app
    refresh();
    return true;
}

void FSFloaterVATsMocap::onClose(bool app_quitting)
{
    // ponytail: closing stops listening (the app keeps listening with its window closed); keep the
    // floater open while capturing.
    if (mRec.active())
        cancelTake("Motion capture take cancelled: the window was closed");
    setListening(false);
}

FSFloaterAnimator* FSFloaterVATsMocap::animator() const
{
    return LLFloaterReg::findTypedInstance<FSFloaterAnimator>("fs_animator");
}

bool FSFloaterVATsMocap::loadData()
{
    if (mRig)
        return true;
    std::string err;
    if (!mSkel.load_dir(gDirUtilp->getExpandedFilename(LL_PATH_CHARACTER, ""), err))
    {
        mDataError = "Could not read the avatar skeleton: " + err;
        return false;
    }
    mRig = std::make_unique<vats::Rig>(mSkel);
    const std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "vats", "");
    if (!vats::parse_rig_table(read_file(dir + "vrm-humanoid.json"), mTable, err))
        mDataError = "app_settings/vats/vrm-humanoid.json: " + (err.empty() ? std::string("missing") : err);
    else if (!vats::parse_face_table(read_file(dir + "face-arkit.json"), mFace, err))
        mDataError = "app_settings/vats/face-arkit.json: " + (err.empty() ? std::string("missing") : err);

    LLComboBox* presets = getChild<LLComboBox>("preset_combo");
    for (const auto& [name, gains] : mFace.presets)
        presets->add(name);
    if (mFace.presets.count("Natural"))  // the preset the default settings match
        presets->setSimple(std::string("Natural"));
    LLComboBox* shapes = getChild<LLComboBox>("shape_combo");
    for (const auto& [shape, motions] : mFace.shapes)
        shapes->add(shape);
    shapes->selectFirstItem();
    return true;
}

void FSFloaterVATsMocap::applyPreset(const std::string& name)
{
    auto it = mFace.presets.find(name);
    if (it == mFace.presets.end())
        return;
    mFaceSettings.gain = it->second.count("*") ? it->second.at("*") : 1.0;
    mFaceSettings.gains.clear();
    for (const auto& [shape, g] : it->second)
        if (shape != "*")
            mFaceSettings.gains[shape] = g / std::max(mFaceSettings.gain, 1e-6);
    getChild<LLUICtrl>("strength_slider")->setValue(mFaceSettings.gain);
    getChild<LLUICtrl>("shape_slider")->setValue(1.0);
}

void FSFloaterVATsMocap::setListening(bool on)
{
    if (!on)
    {
        if (mRec.active())
            cancelTake("Motion capture take cancelled: listening stopped");
        mSock.close();
        mHaveData = false;
        mState = {};
        mPps = mCount = 0;
        stopLive();
    }
    else if (!mSock.is_open())
    {
        mError.clear();
        mSock.open(mPort, mLan[mSource], mError);
        mWindowStart = mListenStart = now_s();
        mRemoteSeen = false;
        mLastPacket = -1;
    }
    updateIdle();
    refresh();
}

// Nothing runs per frame unless a socket is open or a firewall job is pending.
void FSFloaterVATsMocap::updateIdle()
{
    const bool need = mSock.is_open() || mDetectJob.valid() || mAllowJob.valid() || !mDetected || mRec.active();
    if (need && !mIdle)
        gIdleCallbacks.addFunction(onIdle, this);
    else if (!need && mIdle)
        gIdleCallbacks.deleteFunction(onIdle, this);
    mIdle = need;
}

void FSFloaterVATsMocap::onIdle(void* self)
{
    static_cast<FSFloaterVATsMocap*>(self)->poll();
}

void FSFloaterVATsMocap::poll()
{
    const double now = now_s();

    // Firewall: detected once without root, and the result of the one-click rule.
    if (!mDetected && !mDetectJob.valid())
        mDetectJob = std::async(std::launch::async, [] { return vats::detect_firewall(vats::system_runner()); });
    if (ready(mDetectJob))
        mFirewall = mDetectJob.get(), mDetected = true;
    if (ready(mAllowJob))
    {
        const int rc = mAllowJob.get();
        mAllowFailed = rc != 0;
        mAllowMessage = rc == 0     ? "Done: your home network can now reach port " + std::to_string(mPort) + "."
                        : rc == 126 ? "Cancelled: nothing was changed."
                                    : "The firewall was not changed (code " + std::to_string(rc) + "). You can run the copied command yourself.";
        if (rc == 0)
            mAllowedPort = mPort;
    }

    if (mSock.is_open())
    {
        std::uint8_t buf[65536];
        std::vector<vats::OscMessage> msgs;
        std::string from;
        for (int n, guard = 0; guard < 4000 && (n = mSock.receive(buf, sizeof buf, &from)) > 0; ++guard)
        {
            try  // packets come from the network: a hostile one is dropped, never fatal
            {
                if (mSource == 2)
                    vats::apply_ifacialmocap(std::string_view(reinterpret_cast<const char*>(buf), size_t(n)), mState);
                else if (mSource == 1)
                {
                    std::string err;
                    if (!vats::apply_rokoko(buf, size_t(n), mState, "", mActor, err))
                        mError = "Rokoko: " + err;
                    else if (mError.rfind("Rokoko: ", 0) == 0)
                        mError.clear();
                }
                else
                {
                    msgs.clear();
                    vats::parse_osc(buf, size_t(n), msgs);
                    for (const vats::OscMessage& m : msgs)
                        vats::apply_vmc(m, mState);
                }
            }
            catch (const std::exception& e)
            {
                mError = std::string("Dropped a packet that could not be read: ") + e.what();
                continue;
            }
            ++mCount;
            mLastPacket = now;
            mSender = from;
            if (from.rfind("127.", 0) != 0)
                mRemoteSeen = true;
        }
        if (!mHaveData && mState.bones.empty() && (!mState.blend.empty() || mState.has_face_head))
            mHaveData = true;  // a face-only sender
        if (!mHaveData && !mState.bones.empty())
        {
            mHaveData = true;
            // VMC: positions from the first frame, T-pose rotations. Rokoko sends each joint in its own axes,
            // so the first frame is the rest until the performer's T-pose is captured.
            mRest = mSource == 1 ? mState : vats::vmc_t_pose(mState);
            mRest.root.rot = vats::Quat{};
        }
        for (const auto& [name, x] : mState.bones)  // bones a sender starts sending later
            if (!mRest.bones.count(name))
                mRest.bones[name] = vats::Xform{ vats::Quat{}, x.pos };
        if (now - mWindowStart >= 1)
            mPps = mCount, mCount = 0, mWindowStart = now;
    }

    // Recording: frames follow wall time at the clip's rate; the Animator's playhead follows the take.
    if (mRec.active())
    {
        FSFloaterAnimator* a = animator();
        if (!a || !a->hasClip() || a->generation() != mTakeGeneration)
            cancelTake("Motion capture take cancelled: the VATs Animator's animation changed");
        else
        {
            const bool more = mRec.feed(now, mState);
            if (!mRec.frames.empty())
                a->showFrame(mRec.from + int(mRec.frames.size()) - 1);
            if (!more)
                commitTake();
        }
    }
    driveLive();
    updateIdle();
}

// The live pose drives the avatar through a second VATsClipMotion at priority 6, above the Animator's
// preview and the AO on the joints it moves. It is local to this viewer: nothing is sent to the region.
void FSFloaterVATsMocap::driveLive()
{
    if (!mDrive || !mHaveData || !mSock.is_open() || !mRig || (mTable.bones.empty() && mFace.shapes.empty()) ||
        !isAgentAvatarValid())
    {
        stopLive();
        return;
    }
    mLive = vats::live_pose(mSkel, mTable, mRest, mState, nullptr, mFaceOn ? &mFace : nullptr, mFaceSettings);
    mLive.priority = 6;
    mLive.loop = false;
    std::vector<std::string> joints;
    for (const auto& [name, track] : mLive.curves)
        if (mSkel.find(name) >= 0 && gAgentAvatarp->getJoint(name))
            joints.push_back(name);
    if (joints.empty())
    {
        stopLive();
        return;
    }
    if (joints == mLiveJoints && mLiveID.notNull())
        return;  // the motion reads mLive every frame

    // The set of driven joints changed (the first data, or face shapes arriving): bind again.
    stopLive();
    VATsClipMotion::Playback& pb = VATsClipMotion::sLive;
    pb.skeleton = &mSkel;
    pb.rig = mRig.get();
    pb.clip = &mLive;
    pb.joints.clear();
    for (const std::string& name : joints)
        pb.joints.push_back({ name, -1, mLive.has_channels(name, vats::kPosChannels) });
    pb.playing = false;
    pb.frame = 0;
    mLiveJoints = joints;
    LLTransactionID transaction;
    transaction.generate();
    mLiveID = transaction.makeAssetID(gAgent.getSecureSessionID());
    gAgentAvatarp->registerMotion(mLiveID, VATsClipMotion::createLive);
    gAgentAvatarp->startMotion(mLiveID);
}

void FSFloaterVATsMocap::stopLive()
{
    if (mLiveID.notNull() && isAgentAvatarValid())
    {
        gAgentAvatarp->stopMotion(mLiveID, true);
        gAgentAvatarp->removeMotion(mLiveID);
    }
    mLiveID.setNull();
    mLiveJoints.clear();
    VATsClipMotion::sLive.clip = nullptr;
    VATsClipMotion::sLive.joints.clear();
}

void FSFloaterVATsMocap::startRecording()
{
    FSFloaterAnimator* a = animator();
    if (!a || !a->hasClip())
        return setStatus("Open or create an animation in the VATs Animator first; takes are recorded into it.");
    mOnly.clear();
    if (mFaceOnly)
    {
        mOnly = mFace.bones();
        if (mSource == 2 && mFaceSettings.head)
            mOnly.push_back("mHead");
    }
    if (mSelectedOnly)
    {
        const std::vector<int> nodes = a->selectedNodes();
        if (nodes.empty())
            return setStatus("Select a joint of each part to record in the VATs Animator's joint list.");
        for (int n : nodes)
        {
            vats::BodyPart part = vats::body_part_of(mSkel, n);
            part.bones.push_back(n);
            for (int b : part.bones)
                if (std::find(mOnly.begin(), mOnly.end(), mSkel[b].name) == mOnly.end())
                    mOnly.push_back(mSkel[b].name);
        }
    }
    mReport.clear();
    mRec.begin(now_s(), mCountdown, a->clip().fps, mFrom, mPunchOut ? mTo : -1);
    mTakeGeneration = a->generation();
    updateIdle();
    refresh();
}

void FSFloaterVATsMocap::commitTake()
{
    std::vector<vats::VmcState> frames = std::move(mRec.frames);
    mRec.frames.clear();
    mRec.stop();
    FSFloaterAnimator* a = animator();
    if (frames.empty() || !a || !a->hasClip())
        return refresh();
    const std::vector<std::string> only = mSelectedOnly || mFaceOnly ? mOnly : std::vector<std::string>{};
    a->mergeTake("Record Motion Capture", [&](vats::Clip& c)
    {
        mReport = vats::merge_recording(c, mSkel, mTable, mRest, frames, mRec.from, only, mClean, nullptr,
                                         mFaceOn ? &mFace : nullptr, mFaceSettings);
    });
    a->showFrame(mRec.from);  // back to the start of the take, ready to play
    setStatus(mReport.empty() ? "Recorded" : "Motion capture: " + mReport[0]);
    refresh();
}

void FSFloaterVATsMocap::cancelTake(const std::string& why)
{
    mRec.stop();
    mRec.frames.clear();
    setStatus(why);
    refresh();
}

void FSFloaterVATsMocap::draw()
{
    refresh();
    LLFloater::draw();
}

// Everything shown follows the state; called every frame while visible.
void FSFloaterVATsMocap::refresh()
{
    const double now = now_s();
    const bool listening = mSock.is_open();
    const bool got = mLastPacket >= 0;
    const bool lan = mLan[mSource];
    auto row = [this](const char* name, int state, const std::string& text)  // 0 done, 1 needs you, 2 not yet
    {
        LLTextBox* t = getChild<LLTextBox>(name);
        t->setColor(state == 0 ? kDone : state == 1 ? kAttention : kNotYet);
        t->setValue("\xE2\x97\x8F " + text);
    };
    auto show = [this](const char* name, bool visible) { getChildView(name)->setVisible(visible); };
    auto enable = [this](const char* name, bool enabled) { getChildView(name)->setEnabled(enabled); };

    // Connect tab
    getChild<LLUICtrl>("intro_text")->setValue(
        mSource == 2 ? "Receives iFacialMocap from an iPhone or iPad with Face ID. Enter the address the app shows, listen, "
                       "then press Connect to iPhone. Face and head only: add a body sender separately."
        : mSource == 1 ? "Receives Rokoko Studio Live. In Studio, add a Custom streaming target with this computer's address, "
                         "this port and the JSON v3 data format."
                       : "Receives the VMC protocol, which webcam and VR tracker apps send (for example XR Animator, VSeeFace or "
                         "VirtualMotionCapture). Point the app's VMC sender at this computer and port.");
    if (now - mAddrsAt > 5)
        mAddrs = vats::lan_addresses(), mAddrsAt = now;  // Wi-Fi can change
    std::string addrs;
    for (const vats::LanAddress& a : mAddrs)
        addrs += (addrs.empty() ? "" : "   ") + a.ip;
    getChild<LLUICtrl>("addr_text")->setValue(addrs.empty() ? "No network address found" : addrs);
    enable("copy_addr_btn", !mAddrs.empty());

    row("listen_row", listening ? 0 : 2, listening ? "Listening on port " + std::to_string(mPort) : "Not listening yet: press Listen below.");
    const bool lan_late = listening && !got && now - mListenStart > 5;
    if (lan)
        row("lan_row", 0, "Other devices can send");
    else
        row("lan_row", lan_late ? 1 : 2, "Only apps on this computer can send. Turn on Allow Other Devices for a phone or headset.");
    show("lan_fix_btn", !lan && lan_late);

    const bool fw_on = mFirewall == vats::Firewall::Ufw || mFirewall == vats::Firewall::Firewalld;
    const bool blocking = fw_on && listening && lan && !mRemoteSeen && mAllowedPort != mPort && now - mListenStart > 5 && !got;
    if (mRemoteSeen)
        row("fw_row", 0, "Firewall: open (a device on your network got through)");
    else if (!mDetected)
        row("fw_row", 2, "Firewall: checking...");
    else if (mFirewall == vats::Firewall::Off)
        row("fw_row", 0, "Firewall: none found, nothing to change");
    else if (mFirewall == vats::Firewall::Unknown)
#if LL_WINDOWS
        row("fw_row", 2, "Firewall: Windows asks the first time you press Listen. If you pressed Cancel, allow the viewer in "
                         "Windows Security > Firewall & network protection > Allow an app through firewall.");
#elif LL_DARWIN
        row("fw_row", 2, "Firewall: macOS asks the first time you press Listen. If you pressed Don't Allow, allow the viewer in "
                         "System Settings > Network > Firewall > Options.");
#else
        row("fw_row", 2, "Firewall: unknown");
#endif
    else if (blocking)
        row("fw_row", 1, "Your firewall (" + std::string(vats::firewall_name(mFirewall)) + ") is probably blocking the phone.");
    else if (mAllowedPort == mPort)
        row("fw_row", 0, "Firewall: your home network may send to port " + std::to_string(mPort));
    else
        row("fw_row", 2, "Firewall: " + std::string(vats::firewall_name(mFirewall)) +
                             " is on. If nothing arrives, you can let your home network in here.");
    show("fw_allow_btn", blocking && !mAddrs.empty());
    show("copy_cmd_btn", blocking && !mAddrs.empty());
    enable("fw_allow_btn", !mAllowJob.valid());
    getChild<LLButton>("fw_allow_btn")->setLabel(mAllowJob.valid() ? std::string("Waiting for Your Password...")
                                                                    : std::string("Allow on My Home Network"));
    LLTextBox* msg = getChild<LLTextBox>("fw_message");
    msg->setValue(mAllowMessage);
    msg->setColor(mAllowFailed ? kAttention : kDone);

    show("phone_row", mSource == 2);
    if (mSource == 2)
        row("phone_row", got ? 0 : 1, got ? "Phone connected" : "Phone address: type the address the iFacialMocap app shows, then press Connect to iPhone.");
    if (!listening)
        row("recv_row", 2, "Receiving: not listening");
    else if (!got)
        row("recv_row", lan_late ? 1 : 2, "Receiving: waiting for the sender...");
    else if (now - mLastPacket > 1)
        row("recv_row", 1, "Receiving: nothing for " + std::to_string(int(now - mLastPacket)) + " s (last from " + mSender + ")");
    else
        row("recv_row", 0, "Receiving " + std::to_string(mPps) + " packets/s from " + mSender);

    for (const char* name : { "source_combo", "port_spin", "lan_check" })
        enable(name, !listening);
    enable("lan_check", !listening && mSource != 2);  // iFacialMocap always runs on a phone
    getChild<LLUICtrl>("lan_check")->setValue(lan);
    getChild<LLUICtrl>("port_spin")->setValue(mPort);
    getChild<LLButton>("listen_btn")->setLabel(getString(listening ? "stop_listening" : "listen"));
    const bool live = listening && got && now - mLastPacket < 1;
    getChild<LLUICtrl>("conn_text")->setValue(
        !listening ? std::string("Not listening")
        : !got     ? std::string("Waiting for a sender...")
        : !live    ? llformat("No data for %.0f s (last: %s)", now - mLastPacket, mSender.c_str())
                   : llformat("%d packets/s from %s", mPps, mSender.c_str()));
    std::string error = mDataError.empty() ? mError : mDataError;
    if (listening && mState.loaded == 0)
        error += (error.empty() ? "" : "\n") + std::string("The sender says no model is loaded.");
    getChild<LLUICtrl>("error_text")->setValue(error);
    show("phone_edit", mSource == 2);
    show("connect_btn", mSource == 2);
    enable("connect_btn", listening && !getChild<LLUICtrl>("phone_edit")->getValue().asString().empty());
    getChild<LLUICtrl>("rest_text")->setValue(std::string("Rest pose: ") +
        (mRestFromPose ? "captured from the performer" : mSource == 1 ? "first frame: stand in a T-pose and capture" : "T-pose (VRM models)") +
        (listening && mSource == 1 && !mActor.empty() ? ", actor " + mActor : ""));
    enable("rest_capture_btn", mHaveData);
    show("rest_reset_btn", mSource == 0);  // a Rokoko joint's T-pose is not the identity
    enable("rest_reset_btn", mHaveData);

    // Face tab
    getChild<LLUICtrl>("shapes_text")->setValue(mState.blend.empty() ? std::string("Shapes: none received")
                                                                      : llformat("Shapes: %d from the sender", int(mState.blend.size())));
    for (const char* name : { "preset_combo", "strength_slider", "eye_slider", "eye_yaw_slider", "eye_pitch_slider",
                              "shape_combo", "shape_slider", "neutral_clear_btn", "head_check" })
        enable(name, mFaceOn && !mFace.shapes.empty());
    enable("neutral_btn", mFaceOn && !mState.blend.empty());
    show("head_check", mSource == 2);

    // Record tab
    FSFloaterAnimator* a = animator();
    const bool target = a && a->hasClip();
    getChild<LLUICtrl>("target_text")->setValue(target ? "Takes are recorded into the animation open in the VATs Animator."
                                                       : "Open or create an animation in the VATs Animator (Avatar > VATs Animator) to record into it.");
    const bool recording = mRec.active();
    for (const char* name : { "from_spin", "current_btn", "punch_check", "countdown_slider", "selected_check", "face_only_check" })
        enable(name, !recording);
    enable("to_spin", !recording && mPunchOut);
    enable("face_only_check", !recording && mFaceOn);
    getChild<LLUICtrl>("selected_check")->setValue(mSelectedOnly);
    getChild<LLUICtrl>("face_only_check")->setValue(mFaceOnly);
    getChild<LLButton>("record_btn")->setLabel(getString(!recording ? "record" : mRec.counting_down(now) ? "cancel" : "stop"));
    enable("record_btn", recording || (target && mHaveData && (!mTable.bones.empty() || !mFace.shapes.empty())));
    getChild<LLUICtrl>("record_text")->setValue(!recording ? std::string()
                                                : mRec.counting_down(now) ? llformat("%d", int(std::ceil(mRec.start - now)))
                                                : llformat("Recording frame %d", mRec.from + std::max(int(mRec.frames.size()) - 1, 0)));
    for (const char* name : { "deg_spin", "mm_spin" })
        enable(name, mClean.reduce);
    std::string report;
    for (const std::string& line : mReport)
        report += "- " + line + "\n";
    getChild<LLUICtrl>("report_text")->setValue(report);
}

void FSFloaterVATsMocap::setStatus(const std::string& text)
{
    getChild<LLUICtrl>("status_text")->setValue(text);
}
