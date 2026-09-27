/**
 * @file fsfloateranimator.cpp
 * @brief Viewport Avatar Toolset in the viewer: open, play, key-edit, save and upload an animation on your
 *        own avatar (VATs spec 09, stages 6b-6d).
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

#include "fsanimgraphctrl.h"
#include "fsanimtimelinectrl.h"
#include "fsjointpose.h"
#include "fsposingmotion.h"
#include "fsvatsclipmotion.h"
#include "llagent.h"
#include "llagentbenefits.h"
#include "llbutton.h"
#include "lldatapacker.h"
#include "lldir.h"
#include "llfloaterperms.h"
#include "llkeyframemotion.h"
#include "llnotificationsutil.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llspinctrl.h"
#include "lluictrlfactory.h"
#include "llviewerassetupload.h"
#include "llviewermenufile.h"
#include "llvoavatarself.h"

#include "vats/anim_convert.h"
#include "vats/anim_file.h"
#include "vats/edit.h"
#include "vats/project.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <set>
#include <tuple>

FSFloaterAnimator::FSFloaterAnimator(const LLSD& key) : LLFloater(key) {}

FSFloaterAnimator::~FSFloaterAnimator() { stop(); }

bool FSFloaterAnimator::postBuild()
{
    auto button = [this](const char* name, std::function<void()> f)
    { getChild<LLUICtrl>(name)->setCommitCallback([f](LLUICtrl*, const LLSD&) { f(); }); };
    button("open_btn", [this] { onOpenFile(); });
    button("new_pose_btn", [this] { onNew(true); });
    button("new_tpose_btn", [this] { onNew(false); });
    button("save_btn", [this] { onSave(false); });
    button("export_btn", [this] { onSave(true); });
    button("play_btn", [this] { onPlayPause(); });
    button("stop_btn", [this] { onStop(); });
    button("loop_check", [this] { onLoop(); });
    button("as_uploaded_check", [this] { if (mPlayable) start(); });
    button("set_key_btn", [this] { onSetKey(); });
    button("delete_key_btn", [this] { onDeleteKey(); });
    button("key_posed_btn", [this] { onKeyPosed(); });
    button("undo_btn", [this] { onUndo(); });
    button("redo_btn", [this] { onRedo(); });
    button("last_frame_spin", [this] { onLastFrame(); });
    button("upload_btn", [this] { onUpload(); });

    mJointList = getChild<LLScrollListCtrl>("joint_list");
    mJointList->setCommitCallback([this](LLUICtrl*, const LLSD&) { refresh(); });

    // The timeline takes the place of its placeholder border.
    LLView* area = getChild<LLView>("timeline_area");
    FSAnimTimelineCtrl::Params p;
    p.name("timeline");
    p.rect(area->getRect());
    p.follows.flags(FOLLOWS_LEFT | FOLLOWS_RIGHT | FOLLOWS_TOP);
    mTimeline = LLUICtrlFactory::create<FSAnimTimelineCtrl>(p);
    addChild(mTimeline);
    mTimeline->lastFrame = [this] { return mHaveClip ? std::max(mClip.end_frame, 1) : 1; };
    mTimeline->currentFrame = [] { return VATsClipMotion::sPlayback.frame; };
    mTimeline->onScrub = [this](double f) { onScrub(f); };
    mTimeline->keyFrames = [this]
    {
        std::set<double> frames;
        if (!mHaveClip)
            return std::vector<double>();
        std::vector<int> nodes = selectedNodes();
        auto add = [&](const std::string& track)
        {
            for (double f : vats::key_frames(mClip, track))
                frames.insert(f);
        };
        if (nodes.empty())
            for (const auto& [track, curves] : mClip.curves)
                add(track);
        for (int n : nodes)
            add(mSkeleton[n].name);
        return std::vector<double>(frames.begin(), frames.end());
    };

    // Pins show as bands on both, for the selected joints (every joint's when none is selected).
    auto pin_bands = [this]
    {
        std::vector<std::pair<double, double>> out;
        if (!mHaveClip)
            return out;
        const std::vector<int> nodes = selectedNodes();
        for (const vats::Pin& p : mClip.pins)
        {
            const int n = mSkeleton.find(p.joint);
            if (nodes.empty() || std::find(nodes.begin(), nodes.end(), n) != nodes.end())
                out.emplace_back(p.from, p.to < 0 ? mClip.end_frame : p.to);
        }
        return out;
    };
    mTimeline->bands = pin_bands;

    // The graph editor (6e) takes the place of its border too; its edits go through the same History.
    LLView* graph_area = getChild<LLView>("graph_area");
    FSAnimGraphCtrl::Params gp;
    gp.name("graph");
    gp.rect(graph_area->getRect());
    gp.follows.flags(FOLLOWS_ALL);
    mGraph = LLUICtrlFactory::create<FSAnimGraphCtrl>(gp);
    addChild(mGraph);
    mGraph->history = &mHistory;
    mGraph->tracks = [this]
    {
        std::vector<std::string> names;
        for (int n : selectedNodes())
            names.push_back(mSkeleton[n].name);
        return names;
    };
    mGraph->currentFrame = [] { return VATsClipMotion::sPlayback.frame; };
    mGraph->onScrub = [this](double f) { onScrub(f); };
    mGraph->changed = [this] { edited(); };
    mGraph->bands = pin_bands;
    button("graph_frame_all_btn", [this] { mGraph->frameAll(); });
    button("graph_frame_sel_btn", [this] { mGraph->frameSelected(); });
    button("graph_delete_btn", [this] { mGraph->deleteSelected(); });
    const std::tuple<const char*, const char*, vats::Tangent> tangents[] = {
        { "tangent_auto_btn", "Auto", vats::Tangent::Auto },
        { "tangent_spline_btn", "Spline", vats::Tangent::Spline },
        { "tangent_plateau_btn", "Plateau", vats::Tangent::Plateau },
        { "tangent_linear_btn", "Linear", vats::Tangent::Linear },
        { "tangent_flat_btn", "Flat", vats::Tangent::Flat },
        { "tangent_stepped_btn", "Stepped", vats::Tangent::Stepped },
        { "tangent_break_btn", "Break", vats::Tangent::Break },
        { "tangent_unify_btn", "Unify", vats::Tangent::Unify } };
    for (const auto& [ctrl, label, t] : tangents)
    {
        const std::string name = label;
        const vats::Tangent tangent = t;
        button(ctrl, [this, name, tangent] { mGraph->applyTangent(tangent, name); });
    }

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
        getChild<LLUICtrl>("frame_text")->setTextArg("[FRAME]", llformat("%.0f", pb.frame));
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
    // Bones and attachment points the avatar has, in skeleton order (collision volumes left out).
    for (int i = 0; i < mSkeleton.volume_start(); ++i)
        if (isAgentAvatarValid() && gAgentAvatarp->getJoint(mSkeleton[i].name))
            mJointList->addSimpleElement(mSkeleton[i].name)->setUserdata(reinterpret_cast<void*>(intptr_t(i)));
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

    std::string err;
    vats::Clip clip;
    if (gDirUtilp->getExtension(path) == "anim")
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
    setClip(std::move(clip), gDirUtilp->getBaseFileName(path, true));
    VATsClipMotion::sPlayback.playing = mPlayable;
    start();
    refresh();
}

// New clip from what the avatar shows now, or from the T-pose. Body and hand bones only: keying the
// face, wings or tail would freeze whatever else animates them.
void FSFloaterAnimator::onNew(bool from_pose)
{
    if (!loadSkeleton() || !isAgentAvatarValid())
        return;
    vats::Clip clip;
    for (int i = 0; i < mSkeleton.joint_count(); ++i)
    {
        const vats::Category c = mSkeleton[i].category;
        if (c != vats::Category::Body && c != vats::Category::Hands)
            continue;
        if (from_pose)
            keyFromAvatar(clip, i, 0);
        else
            vats::key_rotation(clip, mSkeleton[i].name, 0, vats::Quat{});
    }
    setClip(std::move(clip), from_pose ? "New pose" : "New animation");
    refresh();
}

void FSFloaterAnimator::setClip(vats::Clip clip, const std::string& name)
{
    stop();
    mClip = std::move(clip);
    mName = name;
    mHaveClip = true;
    ++mGeneration;
    mHistory.clear();
    mGraph->clip = &mClip;
    mGraph->clipReplaced(true);
    VATsClipMotion::Playback& pb = VATsClipMotion::sPlayback;
    pb.frame = 0;
    pb.playing = false;
    getChild<LLUICtrl>("file_text")->setValue(name);
    getChild<LLUICtrl>("name_edit")->setValue(name);
    getChild<LLUICtrl>("loop_check")->setValue(mClip.loop);
    if (rebind())
        setStatus(llformat("%d frames at %d fps", mClip.end_frame, mClip.fps));
    start();
}

// What the exported .anim animates is what the preview animates (spec 09 risk 3: refuse, don't twist).
bool FSFloaterAnimator::rebind()
{
    mPlayable = false;
    vats::AnimExportResult out = vats::export_anim(mSkeleton, mClip, {});
    if (!out.errors.empty())
    {
        setStatus("Not playable yet: " + out.errors[0]);
        return false;
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
        return false;
    }
    for (const auto& [name, position] : mPreviewJoints)
    {
        auto it = std::find_if(joints.begin(), joints.end(), [&](const VATsClipMotion::Joint& j) { return j.name == name; });
        if (it != joints.end())
            it->position = it->position || position;
        else if (isAgentAvatarValid() && gAgentAvatarp->getJoint(name))
            joints.push_back({ name, -1, position });
    }
    VATsClipMotion::Playback& pb = VATsClipMotion::sPlayback;
    pb.skeleton = &mSkeleton;
    pb.rig = mRig.get();
    pb.clip = &mClip;
    pb.joints = std::move(joints);
    mPlayable = true;
    return true;
}

template <class F> void FSFloaterAnimator::edit(const std::string& label, F&& change)
{
    mHistory.begin(mClip);
    change(mClip);
    if (mHistory.commit(label, mClip))
        edited();
}

void FSFloaterAnimator::setPreviewJoints(std::vector<std::pair<std::string, bool>> joints)
{
    if (joints == mPreviewJoints)
        return;
    mPreviewJoints = std::move(joints);
    if (!mHaveClip)
        return;
    VATsClipMotion::Playback& pb = VATsClipMotion::sPlayback;
    const bool playing = pb.playing && previewing();
    if (rebind())
    {
        pb.playing = playing;
        start();
    }
}

void FSFloaterAnimator::edited()
{
    ++mEdits;
    // The joints an edit animates can change, so the preview binds again from the frame shown.
    VATsClipMotion::Playback& pb = VATsClipMotion::sPlayback;
    const bool playing = pb.playing && mMotionID.notNull() && !mMotionIsKeyframe;
    if (rebind())
    {
        pb.playing = playing;
        start();
    }
    else
        stop();
    refresh();
}

int FSFloaterAnimator::currentFrame() const
{
    return int(std::lround(VATsClipMotion::sPlayback.frame));
}

std::vector<int> FSFloaterAnimator::selectedNodes() const
{
    std::vector<int> nodes;
    for (LLScrollListItem* item : mJointList->getAllSelected())
        nodes.push_back(int(reinterpret_cast<intptr_t>(item->getUserdata())));
    return nodes;
}

// Keys a joint as the avatar shows it: the inverse of VATsClipMotion's rest x pose rotation. Position too
// when the track already has position keys.
bool FSFloaterAnimator::keyFromAvatar(vats::Clip& clip, int node, int frame) const
{
    const vats::Node& n = mSkeleton[node];
    LLJoint* joint = isAgentAvatarValid() ? gAgentAvatarp->getJoint(n.name) : nullptr;
    if (!joint)
        return false;
    const LLQuaternion r = joint->getRotation();
    vats::Quat shown;
    shown.w = r.mQ[VW], shown.x = r.mQ[VX], shown.y = r.mQ[VY], shown.z = r.mQ[VZ];
    vats::key_rotation(clip, n.name, frame, (n.rest.conj() * shown).normalized());
    if (clip.has_channels(n.name, vats::kPosChannels))
    {
        const LLVector3 p = joint->getPosition();
        const vats::Vec3 rest = node == 0 ? vats::Vec3{} : n.pos;
        vats::key_offset(clip, n.name, frame, vats::Vec3{ p.mV[VX] - rest.x, p.mV[VY] - rest.y, p.mV[VZ] - rest.z });
    }
    return true;
}

vats::Pose FSFloaterAnimator::shownPose() const
{
    vats::Pose pose(mSkeleton.size());
    for (int i = 0; i < mSkeleton.volume_start(); ++i)
    {
        const vats::Node& n = mSkeleton[i];
        LLJoint* joint = isAgentAvatarValid() ? gAgentAvatarp->getJoint(n.name) : nullptr;
        if (!joint)
            continue;
        const LLQuaternion r = joint->getRotation();
        vats::Quat shown;
        shown.w = r.mQ[VW], shown.x = r.mQ[VX], shown.y = r.mQ[VY], shown.z = r.mQ[VZ];
        pose.rot[i] = (n.rest.conj() * shown).normalized();
        const LLVector3 p = joint->getPosition();
        const vats::Vec3 rest = i == 0 ? vats::Vec3{} : n.pos;
        pose.offset[i] = vats::Vec3{ p.mV[VX] - rest.x, p.mV[VY] - rest.y, p.mV[VZ] - rest.z };
    }
    return pose;
}

void FSFloaterAnimator::onSetKey()
{
    const std::vector<int> nodes = selectedNodes();
    if (!mHaveClip || nodes.empty())
        return setStatus("Select joints in the list, then Set Key keys them as your avatar shows them now.");
    const int f = currentFrame();
    edit("Set Key", [&](vats::Clip& c) { for (int n : nodes) keyFromAvatar(c, n, f); });
    setStatus(llformat("Keyed %d joint(s) at frame %d", int(nodes.size()), f));
}

void FSFloaterAnimator::onDeleteKey()
{
    if (!mHaveClip)
        return;
    const int f = currentFrame();
    const std::vector<int> nodes = selectedNodes();
    int removed = 0;
    edit("Delete Key", [&](vats::Clip& c)
    {
        if (nodes.empty())
            removed = vats::delete_keys_at_all(c, f);
        for (int n : nodes)
            removed += vats::delete_keys_at(c, mSkeleton[n].name, f);
        vats::prune(c);
    });
    setStatus(llformat("Removed %d key(s) at frame %d", removed, f));
}

// Keys every joint Firestorm's Poser has changed (its FSPosingMotion marks them), as the avatar shows them.
// This is how the Poser's gizmos and its saved poses become VATs keys: pose or load a pose in the Poser,
// then press this.
void FSFloaterAnimator::onKeyPosed()
{
    if (!mHaveClip || !isAgentAvatarValid())
        return;
    FSPosingMotion* posing = nullptr;
    for (LLMotion* m : gAgentAvatarp->getMotionController().getActiveMotions())
        if ((posing = dynamic_cast<FSPosingMotion*>(m)))
            break;
    if (!posing)
        return setStatus("The Firestorm Poser is not posing you. Open it (Avatar > Poser), pose, then press Key Posed Joints.");
    std::vector<int> nodes;
    for (int i = 0; i < mSkeleton.volume_start(); ++i)
    {
        FSJointPose* pose = posing->getJointPoseByJointName(mSkeleton[i].name);
        if (pose && pose->getJointModified())
            nodes.push_back(i);
    }
    if (nodes.empty())
        return setStatus("The Poser has not changed any joint yet.");
    const int f = currentFrame();
    edit("Key Posed Joints", [&](vats::Clip& c) { for (int n : nodes) keyFromAvatar(c, n, f); });
    setStatus(llformat("Keyed %d posed joint(s) at frame %d. Stop posing in the Poser to see the animation play.",
                       int(nodes.size()), f));
}

void FSFloaterAnimator::onUndo()
{
    if (!mHistory.can_undo())
        return;
    const std::string label = mHistory.undo_label();
    mClip = mHistory.undo();
    mGraph->clipReplaced(false);
    edited();
    setStatus("Undid " + label);
}

void FSFloaterAnimator::onRedo()
{
    if (!mHistory.can_redo())
        return;
    const std::string label = mHistory.redo_label();
    mClip = mHistory.redo();
    mGraph->clipReplaced(false);
    edited();
    setStatus("Redid " + label);
}

void FSFloaterAnimator::onLastFrame()
{
    if (!mHaveClip)
        return;
    const int last = std::clamp(getChild<LLSpinCtrl>("last_frame_spin")->getValue().asInteger(), 1, 3600);
    edit("Length", [&](vats::Clip& c)
    {
        const bool loop_to_end = c.loop_out >= c.end_frame;
        c.end_frame = last;
        c.loop_out = loop_to_end ? last : std::min(c.loop_out, last);
        c.loop_in = std::min(c.loop_in, c.loop_out);
    });
}

void FSFloaterAnimator::onSave(bool as_anim)
{
    if (!mHaveClip)
        return;
    LLFilePickerReplyThread::startPicker([this, as_anim](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
                                         { onSaveChosen(files, as_anim); },
                                         as_anim ? LLFilePicker::FFSAVE_ANIM : LLFilePicker::FFSAVE_ALL,
                                         mName + (as_anim ? ".anim" : ".vat"));
}

void FSFloaterAnimator::onSaveChosen(const std::vector<std::string>& files, bool as_anim)
{
    if (files.empty())
        return;
    std::string bytes;
    if (as_anim)
    {
        vats::AnimExportResult out = vats::export_anim(mSkeleton, mClip, {});
        if (!out.errors.empty())
            return setStatus("Cannot export: " + out.errors[0]);
        const std::vector<std::uint8_t> anim = vats::write_anim(out.file);
        bytes.assign(anim.begin(), anim.end());
    }
    else
    {
        vats::Project project;
        project.clip = mClip;
        bytes = vats::save_project(project);
    }
    std::ofstream out(files[0], std::ios::binary);
    if (!out.write(bytes.data(), std::streamsize(bytes.size())))
        return setStatus("Could not write " + files[0]);
    setStatus("Saved " + files[0]);
}

void FSFloaterAnimator::onPlayPause()
{
    if (!mHaveClip || !mPlayable)
        return;
    VATsClipMotion::Playback& pb = VATsClipMotion::sPlayback;
    if (mMotionID.isNull())
    {
        pb.playing = true;
        start();
        refresh();
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

void FSFloaterAnimator::mergeTake(const std::string& label, const std::function<void(vats::Clip&)>& change)
{
    edit(label, change);
}

void FSFloaterAnimator::showFrame(double frame)
{
    VATsClipMotion::Playback& pb = VATsClipMotion::sPlayback;
    pb.playing = false;
    pb.frame = std::max(frame, 0.0);
    refresh();
}

void FSFloaterAnimator::onStop()
{
    stop();
    VATsClipMotion::sPlayback.frame = 0;
    VATsClipMotion::sPlayback.playing = false;
    refresh();
}

void FSFloaterAnimator::onScrub(double frame)
{
    if (!mHaveClip)
        return;
    VATsClipMotion::Playback& pb = VATsClipMotion::sPlayback;
    pb.playing = false;
    pb.frame = frame;
    if (mPlayable && (mMotionID.isNull() || mMotionIsKeyframe))
    {
        getChild<LLUICtrl>("as_uploaded_check")->setValue(false);
        start();
    }
    refresh();
}

void FSFloaterAnimator::onLoop()
{
    const bool loop = getChild<LLUICtrl>("loop_check")->getValue().asBoolean();
    edit("Loop", [&](vats::Clip& c) { c.loop = loop; });
}

// Upload (spec 09 section 4): the exact bytes the preview-as-uploaded plays, through the viewer's own
// upload path, after the standard cost confirmation.
void FSFloaterAnimator::onUpload()
{
    if (!mHaveClip)
        return;
    const std::string name = getChild<LLUICtrl>("name_edit")->getValue().asString();
    if (name.find_first_not_of(' ') == std::string::npos)
        return setStatus("Give the animation a name before uploading.");
    vats::AnimExportResult out = vats::export_anim(mSkeleton, mClip, {});
    std::vector<std::string> problems = out.errors;
    if (problems.empty())
        problems = vats::validate_anim(out.file, mSkeleton, true);  // the 60 s and 250000-byte limits and the rest
    if (!problems.empty())
        return setStatus("Cannot upload: " + problems[0]);
    const std::vector<std::uint8_t> anim = vats::write_anim(out.file);
    mUploadBytes.assign(anim.begin(), anim.end());
    LLSD args;
    args["PRICE"] = LLAgentBenefitsMgr::current().getAnimationUploadCost();
    LLNotificationsUtil::add("UploadCostConfirmation", args, LLSD(),
                             [this](const LLSD& n, const LLSD& r) { onUploadConfirmed(n, r); });
}

void FSFloaterAnimator::onUploadConfirmed(const LLSD& notification, const LLSD& response)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) != 0 || mUploadBytes.empty())
        return;
    const std::string name = getChild<LLUICtrl>("name_edit")->getValue().asString();
    const std::string desc = getChild<LLUICtrl>("desc_edit")->getValue().asString();
    LLResourceUploadInfo::ptr_t info(std::make_shared<LLNewBufferedResourceUploadInfo>(
        mUploadBytes, LLUUID::null, name, desc, 0, LLFolderType::FT_NONE, LLInventoryType::IT_ANIMATION,
        LLAssetType::AT_ANIMATION, LLFloaterPerms::getNextOwnerPerms("Uploads"), LLFloaterPerms::getGroupPerms("Uploads"),
        LLFloaterPerms::getEveryonePerms("Uploads"), LLAgentBenefitsMgr::current().getAnimationUploadCost(), LLUUID::null,
        true, nullptr, nullptr));
    upload_new_resource(info);
    mUploadBytes.clear();
    setStatus("Uploading " + name + "; it appears in your inventory when the upload finishes.");
}

void FSFloaterAnimator::start()
{
    stop();
    if (!mHaveClip || !mPlayable || !isAgentAvatarValid())
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
}

void FSFloaterAnimator::refresh()
{
    const VATsClipMotion::Playback& pb = VATsClipMotion::sPlayback;
    const bool playing = mMotionID.notNull() && (mMotionIsKeyframe || pb.playing);
    getChild<LLButton>("play_btn")->setLabel(playing ? getString("pause") : getString("play"));
    for (const char* name : { "play_btn", "loop_check", "as_uploaded_check" })
        getChildView(name)->setEnabled(mHaveClip && mPlayable);
    getChildView("stop_btn")->setEnabled(mMotionID.notNull());
    for (const char* name : { "save_btn", "export_btn", "set_key_btn", "delete_key_btn", "key_posed_btn",
                              "last_frame_spin", "upload_btn", "name_edit", "desc_edit" })
        getChildView(name)->setEnabled(mHaveClip);
    getChildView("undo_btn")->setEnabled(mHistory.can_undo());
    getChildView("redo_btn")->setEnabled(mHistory.can_redo());
    getChild<LLUICtrl>("last_frame_spin")->setValue(mHaveClip ? mClip.end_frame : 30);
    getChild<LLUICtrl>("frame_text")->setTextArg("[FRAME]", llformat("%.0f", pb.frame));
    getChild<LLUICtrl>("priority_text")->setTextArg("[PRIORITY]", mHaveClip ? llformat("%d", mClip.priority) : std::string("-"));
    getChild<LLButton>("upload_btn")->setLabelArg("[COST]", llformat("%d", LLAgentBenefitsMgr::current().getAnimationUploadCost()));
}

void FSFloaterAnimator::setStatus(const std::string& text)
{
    getChild<LLUICtrl>("status_text")->setValue(text);
}
