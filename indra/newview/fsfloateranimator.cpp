/**
 * @file fsfloateranimator.cpp
 * @brief Viewport Avatar Toolset in the viewer: open a .vat or .anim and play it on your avatar
 *        (VATs spec 09, stage 6b).
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

#include "fsfloateranimator.h"

#include "fsvatsclipmotion.h"
#include "llagent.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "lldatapacker.h"
#include "lldir.h"
#include "llkeyframemotion.h"
#include "llsliderctrl.h"
#include "llviewermenufile.h"
#include "llvoavatarself.h"

#include "vats/anim_convert.h"
#include "vats/anim_file.h"
#include "vats/project.h"

#include <fstream>
#include <iterator>

FSFloaterAnimator::FSFloaterAnimator(const LLSD& key) : LLFloater(key) {}

FSFloaterAnimator::~FSFloaterAnimator() { stop(); }

bool FSFloaterAnimator::postBuild()
{
    mScrub = getChild<LLSliderCtrl>("scrub");
    getChild<LLUICtrl>("open_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onOpenFile(); });
    getChild<LLUICtrl>("play_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPlayPause(); });
    getChild<LLUICtrl>("stop_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onStop(); });
    getChild<LLUICtrl>("loop_check")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onLoop(); });
    getChild<LLUICtrl>("as_uploaded_check")->setCommitCallback([this](LLUICtrl*, const LLSD&) { if (mHaveClip) start(); });
    mScrub->setCommitCallback([this](LLUICtrl*, const LLSD&) { onScrub(); });
    refresh();
    return true;
}

void FSFloaterAnimator::onClose(bool app_quitting)
{
    stop();
}

void FSFloaterAnimator::draw()
{
    VATsClipMotion::Playback& pb = VATsClipMotion::sPlayback;
    if (pb.finished)  // a non-looping clip reached its end: show it and offer Play again
    {
        pb.finished = false;
        refresh();
    }
    if (mHaveClip && mMotionID.notNull() && !mMotionIsKeyframe && pb.playing)
    {
        mScrub->setValue(F32(pb.frame));
        getChild<LLUICtrl>("frame_text")->setTextArg("[FRAME]", llformat("%.0f", pb.frame));
    }
    LLFloater::draw();
}

// The VATs skeleton comes from the same files the avatar is built from (spec 09 section 2).
bool FSFloaterAnimator::loadSkeleton()
{
    if (mRig)
        return true;
    std::string err;
    if (!mSkeleton.load_dir(gDirUtilp->getExpandedFilename(LL_PATH_CHARACTER, ""), err))
    {
        setStatus("Could not read the avatar skeleton: " + err);
        return false;
    }
    mRig = std::make_unique<vats::Rig>(mSkeleton);
    return true;
}

void FSFloaterAnimator::onOpenFile()
{
    // ponytail: FFLOAD_ALL, since a .vat/.anim filter needs a new ELoadFilter on every platform picker.
    LLFilePickerReplyThread::startPicker([this](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
                                         { onFileChosen(files); },
                                         LLFilePicker::FFLOAD_ALL, false);
}

void FSFloaterAnimator::onFileChosen(const std::vector<std::string>& files)
{
    if (files.empty() || !loadSkeleton())
        return;
    const std::string& path = files[0];
    std::ifstream in(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in && bytes.empty())
    {
        setStatus("Could not read " + path);
        return;
    }

    stop();
    std::string err;
    vats::Clip clip;
    const std::string ext = gDirUtilp->getExtension(path);
    if (ext == "anim")
    {
        vats::AnimFile file;
        if (!vats::parse_anim(std::vector<std::uint8_t>(bytes.begin(), bytes.end()), file, err))
        {
            setStatus("Not a readable .anim: " + err);
            return;
        }
        clip = vats::import_anim(mSkeleton, file).clip;
    }
    else
    {
        vats::Project project;
        if (!vats::load_project(bytes, project, err, path))
        {
            setStatus("Not a readable VATs project: " + err);
            return;
        }
        clip = std::move(project.clip);
    }

    // What the exported .anim animates is what the preview animates (spec 09 risk 3: refuse, don't twist).
    vats::AnimExportResult out = vats::export_anim(mSkeleton, clip, {});
    if (!out.errors.empty())
    {
        setStatus("Cannot play: " + out.errors[0]);
        return;
    }
    std::vector<VATsClipMotion::Joint> joints;
    std::string missing;
    for (const vats::AnimJoint& j : out.file.joints)
    {
        if (!isAgentAvatarValid() || !gAgentAvatarp->getJoint(j.name))
            missing += (missing.empty() ? "" : ", ") + j.name;
        joints.push_back({ j.name, j.priority, !j.pos.empty() });
    }
    if (!missing.empty())
    {
        setStatus("Not played: your avatar has no joint named " + missing);
        return;
    }

    mClip = std::move(clip);
    mHaveClip = true;
    VATsClipMotion::Playback& pb = VATsClipMotion::sPlayback;
    pb.skeleton = &mSkeleton;
    pb.rig = mRig.get();
    pb.clip = &mClip;
    pb.joints = std::move(joints);
    pb.frame = 0;
    pb.playing = true;
    getChild<LLUICtrl>("file_text")->setValue(gDirUtilp->getBaseFileName(path));
    getChild<LLUICtrl>("loop_check")->setValue(mClip.loop);
    start();
    refresh();
    setStatus(llformat("%d frames at %d fps", mClip.end_frame, mClip.fps));
}

void FSFloaterAnimator::start()
{
    stop();
    if (!mHaveClip || !isAgentAvatarValid())
        return;

    LLTransactionID transaction;
    transaction.generate();
    mMotionID = transaction.makeAssetID(gAgent.getSecureSessionID());
    mMotionIsKeyframe = getChild<LLUICtrl>("as_uploaded_check")->getValue().asBoolean();

    if (mMotionIsKeyframe)
    {
        // The exact bytes an upload would send, played by the viewer's own decoder (spec 09 section 3).
        vats::AnimExportResult out = vats::export_anim(mSkeleton, mClip, {});
        std::vector<std::uint8_t> bytes = vats::write_anim(out.file);
        LLKeyframeMotion* motion = dynamic_cast<LLKeyframeMotion*>(gAgentAvatarp->createMotion(mMotionID));
        LLDataPackerBinaryBuffer dp(bytes.data(), S32(bytes.size()));
        if (!motion || !out.errors.empty() || !motion->deserialize(dp, mMotionID, false))
        {
            setStatus("The viewer could not decode the exported animation");
            stop();
            return;
        }
        gAgentAvatarp->startMotion(mMotionID);
    }
    else
    {
        VATsClipMotion::sPlayback.resync = true;
        gAgentAvatarp->registerMotion(mMotionID, VATsClipMotion::create);
        gAgentAvatarp->startMotion(mMotionID);
    }
    LL_INFOS("VATsAnimator") << "preview started: " << mClip.end_frame << " frames at " << mClip.fps << " fps, priority "
                              << mClip.priority << ", " << VATsClipMotion::sPlayback.joints.size() << " joints, "
                              << (mMotionIsKeyframe ? "as uploaded" : "live") << LL_ENDL;
    refresh();
}

void FSFloaterAnimator::stop()
{
    if (mMotionID.isNull())
        return;
    if (isAgentAvatarValid())
    {
        gAgentAvatarp->stopMotion(mMotionID, true);
        gAgentAvatarp->removeMotion(mMotionID);
    }
    if (mMotionIsKeyframe)
        LLKeyframeDataCache::removeKeyframeData(mMotionID);
    mMotionID.setNull();
    refresh();
}

void FSFloaterAnimator::onPlayPause()
{
    if (!mHaveClip)
        return;
    VATsClipMotion::Playback& pb = VATsClipMotion::sPlayback;
    if (mMotionID.isNull())
    {
        pb.playing = true;
        start();
        return;
    }
    if (mMotionIsKeyframe)
        return;  // the viewer's keyframe motion only plays
    if (!pb.playing && !mClip.loop && pb.frame >= std::max(mClip.end_frame, 1))
        pb.frame = 0;  // Play after the end starts over
    pb.playing = !pb.playing;
    pb.resync = pb.playing;
    refresh();
}

void FSFloaterAnimator::onStop()
{
    stop();
    VATsClipMotion::sPlayback.frame = 0;
    refresh();
}

void FSFloaterAnimator::onScrub()
{
    if (!mHaveClip)
        return;
    VATsClipMotion::Playback& pb = VATsClipMotion::sPlayback;
    pb.playing = false;
    pb.frame = mScrub->getValueF32();
    if (mMotionID.isNull() || mMotionIsKeyframe)
    {
        getChild<LLUICtrl>("as_uploaded_check")->setValue(false);
        start();
    }
    refresh();
}

void FSFloaterAnimator::onLoop()
{
    mClip.loop = getChild<LLUICtrl>("loop_check")->getValue().asBoolean();
    if (mMotionID.notNull() && mMotionIsKeyframe)
        start();  // the keyframe motion took its loop flag from the bytes
}

void FSFloaterAnimator::refresh()
{
    const VATsClipMotion::Playback& pb = VATsClipMotion::sPlayback;
    const bool playing = mMotionID.notNull() && (mMotionIsKeyframe || pb.playing);
    getChild<LLButton>("play_btn")->setLabel(playing ? getString("pause") : getString("play"));
    getChildView("play_btn")->setEnabled(mHaveClip);
    getChildView("stop_btn")->setEnabled(mMotionID.notNull());
    getChildView("loop_check")->setEnabled(mHaveClip);
    mScrub->setEnabled(mHaveClip);
    mScrub->setMaxValue(F32(mHaveClip ? std::max(mClip.end_frame, 1) : 1));
    mScrub->setValue(F32(pb.frame));
    getChild<LLUICtrl>("frame_text")->setTextArg("[FRAME]", llformat("%.0f", pb.frame));
    getChild<LLUICtrl>("priority_text")->setTextArg("[PRIORITY]", mHaveClip ? llformat("%d", mClip.priority) : std::string("-"));
}

void FSFloaterAnimator::setStatus(const std::string& text)
{
    getChild<LLUICtrl>("status_text")->setValue(text);
}
