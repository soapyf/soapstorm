/**
 * @file fsvatshost.cpp
 * @brief The VATs editor in the viewer: vats_ui over the world through a viewer Host (VATs spec 09, stage U3).
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

#include "fsvatshost.h"

// VATs' editor UI: its enums have a member named None, which the precompiled header's X11 defines as a macro.
#pragma push_macro("None")
#undef None
#include "app.h"
#include "theme.h"
#pragma pop_macro("None")

#include "fsvatsclipmotion.h"
#include "fsvatsimgui.h"
#include "llagent.h"
#include "llagentbenefits.h"
#include "llagentcamera.h"
#include "llanimationstates.h"
#include "llappviewer.h"
#include "llaudioengine.h"
#include "lldir.h"
#include "lldirpicker.h"
#include "llfile.h"
#include "llfloaterperms.h"
#include "llnotificationsutil.h"
#include "llstartup.h"
#include "lluicolortable.h"
#include "llversioninfo.h"
#include "lltimer.h"
#include "llviewerassetupload.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewermenufile.h"
#include "llvoavatarself.h"
#include "llweb.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <memory>
#include <set>

namespace
{
    using vats::Vec3;

    Vec3 toVATs(const LLVector3& v) { return { v.mV[VX], v.mV[VY], v.mV[VZ] }; }
    LLVector3 toLL(const Vec3& v) { return LLVector3(F32(v.x), F32(v.y), F32(v.z)); }

    bool sameJoints(const std::vector<VATsClipMotion::Joint>& a, const std::vector<VATsClipMotion::Joint>& b)
    {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const auto& x, const auto& y)
                          { return x.name == y.name && x.position == y.position; });
    }

    ImVec4 toImGui(const LLColor4& c) { return ImVec4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], 1.f); }
    // A skin colour as seen over `under` (floater colours are partly transparent).
    LLColor4 over(const LLColor4& c, const LLColor4& under) { return lerp(under, c, c.mV[VALPHA]); }

    // Samples as a 16-bit PCM WAV, the format the audio engine's decoded-sound cache holds (stage 6g).
    std::string wavBytes(const float* pcm, size_t frames, int channels_in, int rate)
    {
        const int channels = std::min(channels_in, 2);  // the engine plays mono and stereo
        std::string wav(44 + frames * channels * 2, '\0');
        auto put = [&](size_t at, U32 v, int bytes) { for (int b = 0; b < bytes; ++b) wav[at + b] = char((v >> (8 * b)) & 0xff); };
        wav.replace(0, 4, "RIFF");
        put(4, U32(wav.size() - 8), 4);
        wav.replace(8, 8, "WAVEfmt ");
        put(16, 16, 4);
        put(20, 1, 2);  // PCM
        put(22, U32(channels), 2);
        put(24, U32(rate), 4);
        put(28, U32(rate * channels * 2), 4);
        put(32, U32(channels * 2), 2);
        put(34, 16, 2);
        wav.replace(36, 4, "data");
        put(40, U32(frames * channels * 2), 4);
        for (size_t f = 0; f < frames; ++f)
            for (int c = 0; c < channels; ++c)
            {
                const float s = std::clamp(pcm[f * channels_in + c], -1.f, 1.f);
                put(44 + (f * channels + c) * 2, U32(U16(S16(std::lround(s * 32767.f)))), 2);
            }
        return wav;
    }

    // The viewer as vats::ui::Host (spec 09 §0b table, §0d). Lives for the session; the editor (App) comes
    // and goes, and close() drops everything that points into it.
    class ViewerHost final : public vats::ui::Host
    {
    public:
        ViewerHost()
        {
            const std::string delim = gDirUtilp->getDirDelimiter();
            const std::string settings = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "vats");
            mPaths.data = settings;                                  // retarget/ rig and motion capture tables
            mPaths.assets = settings + delim + "assets";             // fonts, starter props, welcome.md
            mPaths.help = settings + delim + "help";                 // the wiki pages
            mPaths.character = gDirUtilp->getExpandedFilename(LL_PATH_CHARACTER, "");  // the avatar's own files
            mPaths.user = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "vats") + delim;
            LLFile::mkdir(mPaths.user);
            mPaths.settings = mPaths.user + "settings.json";
        }

        const vats::ui::Paths& paths() const override { return mPaths; }

        // The world is the view: no scene is built. Thumbnails too: the UI shows its plain icons instead.
        bool scene_begin(vats::ui::SceneTarget, int, int, const vats::Camera&, const vats::SceneColours&,
                         const vats::Mat4*) override { return false; }
        void scene_ground(const Vec3&) override {}
        void scene_triangles(const std::vector<vats::Vertex>&, const std::vector<std::uint32_t>&, bool, float, bool) override {}
        ImTextureID scene_end() override { return ImTextureID{}; }
        bool save_thumbnail_png(const std::string&) override { return false; }
        ImTextureID load_texture(const std::string&) override { return ImTextureID{}; }
        void free_texture(ImTextureID) override {}

        void drive_avatar(const vats::Skeleton& skel, const vats::Pose& pose, const vats::Clip& clip, double) override;

        vats::Camera& camera() override { return mCamera; }
        // The world view fills ImGui's whole display (FSVATsImGui maps it onto LLViewerCamera's rectangle).
        vats::Projector projector(ImVec2, ImVec2) override
        {
            vats::Projector p;
            p.view_proj = mViewProj;
            p.w = std::max(ImGui::GetIO().DisplaySize.x, 1.f);
            p.h = std::max(ImGui::GetIO().DisplaySize.y, 1.f);
            return p;
        }

        void open_file_dialog(const std::vector<vats::ui::FileFilter>&, bool multiple, vats::ui::FilesChosen done) override
        {
            // ponytail: FFLOAD_ALL, since VATs' filters (.vat, .hxanim, glTF, audio...) have no ELoadFilter.
            LLFilePickerReplyThread::startPicker(
                [done](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter) { done(files); },
                LLFilePicker::FFLOAD_ALL, multiple,
                [done](const std::vector<std::string>&, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter) { done({}); });
        }
        void save_file_dialog(const std::vector<vats::ui::FileFilter>&, const std::string& suggested, vats::ui::FilesChosen done) override
        {
            // The picker proposes a file name; the UI adds the extension itself.
            LLFilePickerReplyThread::startPicker(
                [done](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter) { done(files); },
                LLFilePicker::FFSAVE_ALL, gDirUtilp->getBaseFileName(suggested),
                [done](const std::vector<std::string>&, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter) { done({}); });
        }
        void open_folder_dialog(const std::string& start, vats::ui::FilesChosen done) override
        {
            // The folder picker has no cancel callback: a cancelled choice simply never answers.
            (new LLDirPickerThread([done](const std::vector<std::string>& dirs, std::string) { done(dirs); }, start))->getFile();
        }
        // An ImGui modal over the editor, drawn after the UI's frame (drawQuestion).
        void ask(const std::string& title, const std::string& text, const std::vector<std::string>& buttons,
                 std::function<void(int)> done) override
        {
            mQuestions.push_back({ title, text, buttons.empty() ? std::vector<std::string>{ "OK" } : buttons, std::move(done) });
        }
        bool associate_file_types(bool, std::string& message) override
        {
            message = "The viewer does not register file types; the Viewport Avatar Toolset app does.";
            return false;
        }

        bool audio_start(int rate, int channels, float gain) override;
        void audio_queue(const float* samples, std::size_t count) override;
        void audio_stop() override { stopSound(); }

        std::uint64_t ticks_ns() const override { return std::uint64_t(LLTimer::getTotalTime().value()) * 1000; }
        void wake(double) override {}  // the viewer draws every frame

        void open_url(const std::string& url) override { LLWeb::loadURLExternal(url); }
        void set_title(const std::string&) override {}  // the editor has no window title of its own
        vats::CommandRunner command_runner() override { return vats::system_runner(); }

        bool world_view() const override { return true; }
        bool pointer_on_world() const override { return FSVATsImGui::pointerOnWorld(); }
        const vats::Shape* body_shape() const override { return mHaveShape ? &mShape : nullptr; }

        bool skin_colours(vats::HostColours& out) const override;
        std::string host_name() const override { return LLVersionInfo::instance().getChannel(); }

        bool can_upload() const override { return true; }
        void upload_anim(const std::vector<std::uint8_t>& bytes, const std::string& name,
                         std::function<void(const std::string&)> done) override;

        void beforeFrame();  // outside the ImGui frame: the world's frame, the worn body, the camera
        void afterFrame();   // inside it, after the UI's frame: questions, camera edits
        void close();        // the editor is going: stop driving the avatar and the sound, drop its callbacks

    private:
        LLJoint* avatarJoint(int node);
        void isolate();      // the avatar belongs to the editor: sit it down once, stop every other motion
        void restore();      // everything back as it was: motions still wanted, and stand up if we sat
        void updateShape();
        void stopMotion();
        void drawQuestion();
        void pushCamera();
        void startSound();
        void stopSound();
        LLVector3 toAgent(const Vec3& p) const { return toLL(p - pelvisRest()) * mRot + mPos; }
        Vec3 fromAgent(const LLVector3& a) const { return toVATs((a - mPos) * ~mRot) + pelvisRest(); }
        Vec3 dirFromAgent(const LLVector3& d) const { return toVATs(d * ~mRot); }
        // VATs' space has its origin at the feet (the pelvis rests 1.067 m up); the viewer's mRoot sits where
        // the pelvis rests.
        Vec3 pelvisRest() const { return mSkel && mSkel->size() ? (*mSkel)[0].pos : Vec3{ 0, 0, 1.067 }; }

        vats::ui::Paths mPaths;
        vats::Camera mCamera, mSynced;  // mSynced: as read from the viewer this frame, to spot the UI's edits
        vats::Mat4 mViewProj;
        LLVector3 mPos;                  // VATs' space in the agent frame: the avatar's mRoot
        LLQuaternion mRot;

        const vats::Skeleton* mSkel = nullptr;
        vats::Pose mPose;               // what VATsClipMotion::sEditor shows
        std::vector<VATsClipMotion::Joint> mJoints;
        std::vector<bool> mDrivesPos;    // by node
        LLUUID mMotionID;
        const LLVOAvatarSelf* mMotionOn = nullptr;
        std::vector<LLJoint*> mJointCache;
        const LLVOAvatarSelf* mCacheFor = nullptr;
        vats::Shape mShape;
        bool mHaveShape = false;

        // Isolation (spec 09 U4): the motions stopped on this avatar while the editor is open, and whether the
        // editor sat the avatar down.
        const LLVOAvatarSelf* mIsolatedOn = nullptr;
        std::set<LLUUID> mStopped;
        bool mWeSat = false;

        struct Question
        {
            std::string title, text;
            std::vector<std::string> buttons;
            std::function<void(int)> done;
        };
        std::deque<Question> mQuestions;

        std::function<void(const std::string&)> mUploadDone;
        std::string mUploadBytes, mUploadName;

        // Audio (AU-3 in the viewer): one WAV in the decoded-sound cache for the file being played, reused
        // with a start offset while the UI queues suffixes of the same samples.
        int mRate = 0, mChannels = 0;
        float mGain = 1.f;
        F64 mLead = 0;                   // seconds of silence the UI queued before the samples
        const float* mWavEnd = nullptr;  // the end of the samples the WAV was made from (identifies the file)
        size_t mWavSamples = 0;
        LLUUID mWavAsset, mSource;
        U32 mOffsetMS = 0;
        F64 mPlayAt = -1;                // when a queued start is due (after its lead silence)
    };

    LLJoint* ViewerHost::avatarJoint(int node)
    {
        if (!mSkel || !isAgentAvatarValid())
            return nullptr;
        if (mCacheFor != gAgentAvatarp.get() || mJointCache.size() != (size_t)mSkel->size())
        {
            mCacheFor = gAgentAvatarp.get();
            mJointCache.assign(mSkel->size(), nullptr);
            for (int i = 0; i < mSkel->size(); ++i)
                mJointCache[i] = gAgentAvatarp->getJoint((*mSkel)[i].name);
        }
        return mJointCache[node];
    }

    // The editor evaluates the pose (clip, IK, pins and its live previews); the avatar shows it through
    // VATsClipMotion. Every joint of the avatar is bound, so the pose is the only one shown: rotations for all
    // bones, positions for the pelvis and every joint the pose moves or the clip keys. Collision volumes and
    // attachment points are bound when the clip keys them or the pose moves them. The set is rebound whenever
    // it changes.
    void ViewerHost::drive_avatar(const vats::Skeleton& skel, const vats::Pose& pose, const vats::Clip& clip, double)
    {
        mSkel = &skel;
        if (!isAgentAvatarValid())
            return stopMotion();
        mPose = pose;
        std::vector<VATsClipMotion::Joint> joints;
        std::vector<bool> drives_pos(skel.size(), false);
        for (int i = 0; i < skel.size() && i < (int)pose.rot.size(); ++i)
        {
            const vats::Node& n = skel[i];
            const auto track = clip.curves.find(n.name);
            const bool keyed = track != clip.curves.end() && !track->second.empty();
            const bool moved = pose.offset[i].length() > 1e-6;
            const bool turned = 1.0 - std::fabs(pose.rot[i].w) > 1e-9;
            const bool bone = i < skel.joint_count() && !n.attachment;
            if ((!bone && !keyed && !moved && !turned) || !avatarJoint(i))
                continue;
            drives_pos[i] = i == 0 || moved || (keyed && clip.has_channels(n.name, vats::kPosChannels));
            joints.push_back({ n.name, drives_pos[i] });
        }
        VATsClipMotion::Playback& pb = VATsClipMotion::sEditor;
        pb.skeleton = &skel;
        pb.pose = &mPose;
        const bool active = mMotionID.notNull() && mMotionOn == gAgentAvatarp.get() && gAgentAvatarp->isMotionActive(mMotionID);
        if (sameJoints(joints, mJoints) && active)
            return;
        stopMotion();
        mJoints = joints;
        mDrivesPos = std::move(drives_pos);
        pb.joints = std::move(joints);
        if (pb.joints.empty())
            return;
        LLTransactionID transaction;
        transaction.generate();
        mMotionID = transaction.makeAssetID(gAgent.getSecureSessionID());
        mMotionOn = gAgentAvatarp.get();
        // Straight to the motion controller: LLVOAvatar::startMotion would offer the id to the AO first.
        gAgentAvatarp->registerMotion(mMotionID, VATsClipMotion::create);
        gAgentAvatarp->getMotionController().startMotion(mMotionID, 0.f);
        LL_INFOS("VATsEditor") << "driving " << pb.joints.size() << " joints of the avatar" << LL_ENDL;
    }

    void ViewerHost::stopMotion()
    {
        if (mMotionID.notNull() && isAgentAvatarValid() && mMotionOn == gAgentAvatarp.get())
        {
            gAgentAvatarp->getMotionController().stopMotionLocally(mMotionID, true);
            gAgentAvatarp->removeMotion(mMotionID);
        }
        mMotionID.setNull();
        mMotionOn = nullptr;
        mJoints.clear();
    }

    // The worn avatar's proportions in VATs' terms (skeleton.h Shape): each bone's scale, and its position
    // less the rest position VATs knows (sliders, mesh joint offsets). A position this editor drives is
    // VATs' own offset, so it is left out; the pelvis sits on the frame the host maps.
    void ViewerHost::updateShape()
    {
        mHaveShape = false;
        if (!mSkel || !isAgentAvatarValid())
            return;
        const vats::Skeleton& skel = *mSkel;
        mShape.scale.assign(skel.size(), Vec3{ 1, 1, 1 });
        mShape.offset.assign(skel.size(), Vec3{});
        for (int i = 0; i < skel.joint_count(); ++i)
        {
            LLJoint* joint = avatarJoint(i);
            if (!joint)
                continue;
            mShape.scale[i] = toVATs(joint->getScale());
            if (i == 0)
                continue;
            Vec3 offset = toVATs(joint->getPosition()) - skel[i].pos;
            if (i < (int)mDrivesPos.size() && mDrivesPos[i] && i < (int)mPose.offset.size())
                offset = offset - mPose.offset[i];
            mShape.offset[i] = offset;
        }
        mHaveShape = true;
    }

    // Spec 09 U4: while the editor is open the avatar is at its whims. Once, it sits down on the ground (the
    // viewer's own Sit Down: AgentUpdate's sit-on-ground flag), so it cannot be walked or bumped; then every frame
    // every other motion on it is stopped locally (AO, server and scripted animations, stands, walks, look-at, eye
    // and head motion, breathing, expressions), straight on the motion controller, so nothing is sent to the
    // region. What the region starts meanwhile is stopped the same way when it arrives.
    void ViewerHost::isolate()
    {
        if (!isAgentAvatarValid() || LLStartUp::getStartupState() < STATE_STARTED)
            return;
        LLVOAvatarSelf* avatar = gAgentAvatarp.get();
        if (mIsolatedOn != avatar)
        {
            mIsolatedOn = avatar;
            mStopped.clear();
            mWeSat = false;
            // Not while flying (the viewer's Sit Down is disabled then) or already seated; RLVa may refuse.
            if (!avatar->isSitting() && !gAgent.getFlying() && !avatar->isEditingAppearance())
            {
                gAgent.sitDown();
                mWeSat = true;
                LL_INFOS("VATsEditor") << "sitting the avatar down on the ground while the editor is open" << LL_ENDL;
            }
        }
        LLMotionController& controller = avatar->getMotionController();
        std::vector<LLUUID> others;
        for (LLMotion* motion : controller.getActiveMotions())
            if (motion && motion->getID() != mMotionID)
                others.push_back(motion->getID());
        for (const LLUUID& id : others)
        {
            controller.stopMotionLocally(id, true);
            mStopped.insert(id);
        }
    }

    // Everything back as it was: every motion stopped here that is still wanted starts again locally (the
    // default ones, and each animation the region still signals, the ones it started meanwhile included), and
    // the avatar stands up if the editor sat it down and it still sits on the ground.
    void ViewerHost::restore()
    {
        if (!mIsolatedOn || !isAgentAvatarValid() || mIsolatedOn != gAgentAvatarp.get())
        {
            mIsolatedOn = nullptr;
            mStopped.clear();
            return;
        }
        LLVOAvatarSelf* avatar = gAgentAvatarp.get();
        std::set<LLUUID> wanted{ ANIM_AGENT_HEAD_ROT, ANIM_AGENT_EYE, ANIM_AGENT_BODY_NOISE, ANIM_AGENT_BREATHE_ROT,
                                 ANIM_AGENT_PHYSICS_MOTION, ANIM_AGENT_HAND_MOTION, ANIM_AGENT_PELVIS_FIX };
        for (const auto& signaled : avatar->mSignaledAnimations)
        {
            wanted.insert(signaled.first);
            wanted.insert(avatar->remapMotionID(signaled.first));
        }
        S32 restarted = 0;
        for (const LLUUID& id : mStopped)
            if (wanted.count(id) && avatar->getMotionController().startMotion(id, 0.f))
                ++restarted;
        const bool leaving = LLApp::isExiting() || LLAppViewer::instance()->quitRequested() || LLAppViewer::instance()->logoutRequestSent();
        if (mWeSat && !leaving && avatar->isSitting() && !avatar->getParent())
            gAgent.standUp();  // the viewer's own Stand Up
        LL_INFOS("VATsEditor") << "avatar handed back: " << restarted << " of " << mStopped.size()
                                << " stopped motions restarted" << (mWeSat ? ", standing up" : "") << LL_ENDL;
        mIsolatedOn = nullptr;
        mStopped.clear();
        mWeSat = false;
    }

    // The viewer's skin as the editor's colours, read from LLUIColorTable (the colours the skin and its theme
    // set up). Skins draw most widgets from images, so frames are the panel colour moved toward the text.
    bool ViewerHost::skin_colours(vats::HostColours& out) const
    {
        const LLUIColorTable& table = LLUIColorTable::instance();
        LLColor4 panel = table.getColor("FloaterFocusBackgroundColor", LLColor4(0.16f, 0.16f, 0.16f, 1.f)).get();
        panel.mV[VALPHA] = 1.f;
        const LLColor4 text = over(table.getColor("LabelTextColor", LLColor4::white).get(), panel);
        const LLColor4 bar = over(table.getColor("MenuBarBgColor", LLColor4::black).get(), panel);
        out.panel = toImGui(panel);
        out.bg = toImGui(bar);
        out.frame = toImGui(lerp(panel, text, 0.10f));
        out.frame_hi = toImGui(lerp(panel, text, 0.18f));
        out.frame_active = toImGui(lerp(panel, text, 0.26f));
        out.text = toImGui(text);
        out.text_dim = toImGui(over(table.getColor("LabelDisabledColor", LLColor4::grey).get(), panel));
        out.border = toImGui(over(table.getColor("FloaterFocusBorderColor", LLColor4::grey).get(), panel));
        out.accent = toImGui(over(table.getColor("EmphasisColor", LLColor4::orange).get(), panel));
        return true;
    }

    void ViewerHost::beforeFrame()
    {
        isolate();
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        if (isAgentAvatarValid() && gAgentAvatarp->getRootJoint())
        {
            LLJoint* root = gAgentAvatarp->getRootJoint();
            mPos = root->getWorldPosition();
            mRot = root->getWorldRotation();
        }
        else
        {
            // No avatar yet (the login screen): VATs' skeleton stands 3.2 m in front of the camera, facing it.
            LLVector3 at = cam->getAtAxis();
            at.mV[VZ] = 0.f;
            if (at.normVec() < 1e-3f)
                at = LLVector3::x_axis;
            mPos = cam->getOrigin() + at * 3.2f;
            mPos.mV[VZ] = cam->getOrigin().mV[VZ] - 0.3f;
            mRot.setAngleAxis(atan2f(-at.mV[VY], -at.mV[VX]), 0.f, 0.f, 1.f);
        }
        updateShape();

        // The viewer's camera is the view: the UI's camera follows it every frame (in VATs' space), and
        // the projection is LLViewerCamera's own, so markers and gizmos sit on the world.
        const Vec3 eye = fromAgent(cam->getOrigin());
        const Vec3 at = dirFromAgent(cam->getAtAxis()).normalized();
        const Vec3 up = dirFromAgent(cam->getUpAxis()).normalized();
        F64 distance = 3.2;
        if (isAgentAvatarValid())
            distance = (gAgent.getPosAgentFromGlobal(gAgentCamera.getFocusGlobal()) - cam->getOrigin()) * cam->getAtAxis();
        mCamera.fov = cam->getView();
        mCamera.distance = llclamp(distance, 0.1, 512.0);
        mCamera.pitch = std::asin(llclamp(-at.z, -1.0, 1.0));
        mCamera.yaw = std::atan2(-at.y, -at.x);
        mCamera.target = eye + at * mCamera.distance;
        mSynced = mCamera;
        mViewProj = vats::perspective(cam->getView(), cam->getAspect(), 0.05, 1024.0) * vats::look_at(eye, eye + at, up);

        if (mPlayAt >= 0 && LLTimer::getTotalSeconds() >= mPlayAt)
            startSound();
    }

    void ViewerHost::afterFrame()
    {
        drawQuestion();
        pushCamera();
    }

    // The UI moved its camera (Frame Selected, the view cube, a camera view, Alt+arrows): the viewer's
    // camera goes there, the way an Alt+click focus does.
    void ViewerHost::pushCamera()
    {
        const bool same = (mCamera.target - mSynced.target).length() < 1e-5 && std::fabs(mCamera.yaw - mSynced.yaw) < 1e-6 &&
                          std::fabs(mCamera.pitch - mSynced.pitch) < 1e-6 && std::fabs(mCamera.distance - mSynced.distance) < 1e-6;
        if (same || !isAgentAvatarValid() || gAgentCamera.cameraMouselook())
            return;
        const LLVector3 eye = toAgent(mCamera.eye()), target = toAgent(mCamera.target);
        gAgentCamera.setFocusOnAvatar(false, false);
        gAgentCamera.setCameraPosAndFocusGlobal(gAgent.getPosGlobalFromAgent(eye), gAgent.getPosGlobalFromAgent(target), LLUUID::null);
    }

    void ViewerHost::drawQuestion()
    {
        if (mQuestions.empty())
            return;
        const char* id = "##vats_host_question";
        if (!ImGui::IsPopupOpen(id))
            ImGui::OpenPopup(id);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        int answer = -1;
        Question& q = mQuestions.front();
        if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            ImGui::TextUnformatted(q.title.c_str());
            ImGui::Separator();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
            ImGui::TextUnformatted(q.text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            for (size_t i = 0; i < q.buttons.size(); ++i)
            {
                if (i)
                    ImGui::SameLine();
                if (ImGui::Button(q.buttons[i].c_str()))
                    answer = (int)i;
            }
            // The first button answers Enter, the last Esc (ui::Host::ask).
            if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
                answer = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                answer = (int)q.buttons.size() - 1;
            if (answer >= 0)
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (answer >= 0)
        {
            std::function<void(int)> done = std::move(q.done);
            mQuestions.pop_front();
            if (done)
                done(answer);
        }
    }

    // Spec 09 section 4: the exact bytes, the standard price confirmation, the viewer's own upload path.
    void ViewerHost::upload_anim(const std::vector<std::uint8_t>& bytes, const std::string& name,
                                 std::function<void(const std::string&)> done)
    {
        if (LLStartUp::getStartupState() < STATE_STARTED)
            return done("Log in to upload");
        if (!mUploadBytes.empty())
            return done("An upload is already waiting for its confirmation");
        mUploadBytes.assign(bytes.begin(), bytes.end());
        mUploadName = name.find_first_not_of(' ') == std::string::npos ? std::string("VATs animation") : name;
        mUploadDone = std::move(done);
        LLSD args;
        args["PRICE"] = LLAgentBenefitsMgr::current().getAnimationUploadCost();
        LLNotificationsUtil::add("UploadCostConfirmation", args, LLSD(), [this](const LLSD& n, const LLSD& r)
        {
            // Taken out first: done may hand over the next upload (the UI uploads several one after another).
            const std::string bytes = std::exchange(mUploadBytes, std::string()), name = mUploadName;
            auto done = std::exchange(mUploadDone, nullptr);
            const bool confirmed = LLNotificationsUtil::getSelectedOption(n, r) == 0;
            if (confirmed)
            {
                LLResourceUploadInfo::ptr_t info(std::make_shared<LLNewBufferedResourceUploadInfo>(
                    bytes, LLUUID::null, name, std::string(), 0, LLFolderType::FT_NONE,
                    LLInventoryType::IT_ANIMATION, LLAssetType::AT_ANIMATION, LLFloaterPerms::getNextOwnerPerms("Uploads"),
                    LLFloaterPerms::getGroupPerms("Uploads"), LLFloaterPerms::getEveryonePerms("Uploads"),
                    LLAgentBenefitsMgr::current().getAnimationUploadCost(), LLUUID::null, true, nullptr, nullptr));
                upload_new_resource(info);
                LL_INFOS("VATsEditor") << "uploading " << name << ", " << bytes.size() << " bytes" << LL_ENDL;
            }
            if (done)  // null once the editor has closed
                done(confirmed ? "Uploading " + name + "; it appears in your inventory when the upload finishes"
                               : "Upload of " + name + " cancelled");
        });
    }

    // --- Audio -------------------------------------------------------------------------------------------

    bool ViewerHost::audio_start(int rate, int channels, float gain)
    {
        stopSound();
        if (!gAudiop || rate <= 0 || channels <= 0)
            return false;
        if (rate != mRate || channels != mChannels)
            mWavEnd = nullptr;  // another file
        mRate = rate, mChannels = channels, mGain = gain, mLead = 0;
        return true;
    }

    // The UI queues from the playhead to the end of the file, after any silence before the audio starts
    // (ui/audio_track.cpp). The WAV is made from the longest such run seen; a shorter one ending at the same
    // samples is the same file, played from an offset. Runs under half a second (the UI's scrub snippets)
    // are skipped: each would be a sound of its own.
    void ViewerHost::audio_queue(const float* samples, std::size_t count)
    {
        if (!gAudiop || !mRate || !count)
            return;
        const size_t frames = count / mChannels;
        if (std::all_of(samples, samples + count, [](float s) { return s == 0.f; }))
        {
            mLead += F64(frames) / mRate;
            return;
        }
        if (F64(frames) / mRate < 0.5)
            return;
        const float* end = samples + count;
        if (end != mWavEnd || count > mWavSamples)
        {
            if (mWavAsset.notNull())
                LLFile::remove(gDirUtilp->getExpandedFilename(LL_PATH_FS_SOUND_CACHE, mWavAsset.asString()) + ".dsf");
            mWavAsset.generate();
            const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_FS_SOUND_CACHE, mWavAsset.asString()) + ".dsf";
            const std::string wav = wavBytes(samples, frames, mChannels, mRate);
            std::ofstream out(path, std::ios::binary);
            if (!out.write(wav.data(), std::streamsize(wav.size())))
            {
                LL_WARNS("VATsEditor") << "could not write " << path << LL_ENDL;
                mWavAsset.setNull();
                mWavEnd = nullptr;
                return;
            }
            mWavEnd = end;
            mWavSamples = count;
        }
        mOffsetMS = U32(F64((mWavSamples - count) / mChannels) * 1000.0 / mRate);
        mPlayAt = LLTimer::getTotalSeconds() + mLead;
        if (mLead <= 0)
            startSound();
    }

    void ViewerHost::startSound()
    {
        mPlayAt = -1;
        if (!gAudiop || mWavAsset.isNull())
            return;
        if (mSource.notNull())
            if (LLAudioSource* old = gAudiop->findAudioSource(mSource))
                gAudiop->cleanupAudioSource(old);
        mSource.generate();
        LLAudioSource* src = new LLAudioSource(mSource, gAgentID, llclamp(mGain, 0.f, 1.f), LLAudioEngine::AUDIO_TYPE_UI);
        gAudiop->addAudioSource(src);
        src->setForcedPriority(true);
        src->setStartOffsetMS(mOffsetMS);
        src->updatePriority();
        src->play(mWavAsset);
    }

    void ViewerHost::stopSound()
    {
        mPlayAt = -1;
        mLead = 0;
        if (gAudiop && mSource.notNull())
            if (LLAudioSource* src = gAudiop->findAudioSource(mSource))
                gAudiop->cleanupAudioSource(src);
        mSource.setNull();
    }

    void ViewerHost::close()
    {
        stopMotion();
        restore();
        stopSound();
        VATsClipMotion::sEditor = VATsClipMotion::Playback();
        mSkel = nullptr;
        mCacheFor = nullptr;
        mHaveShape = false;
        mQuestions.clear();
        mUploadDone = nullptr;  // the confirmation may still come; it uploads, nobody is told
        mWavEnd = nullptr;
    }

    // --- The editor --------------------------------------------------------------------------------------

    constexpr const char* CLIENT = "vats_editor";
    std::unique_ptr<ViewerHost> sHost;   // kept for the session once made
    std::unique_ptr<vats::App> sApp;    // while the editor is open
    bool sOpen = false;                  // registered with FSVATsImGui
    bool sQuit = false;                  // the UI chose to quit (App::frame returned false)
    bool sClosing = false;               // unticked in the menu: the UI was asked to quit
    bool sFailed = false;

    void editorBefore()
    {
        if (!sApp && !sFailed)
        {
            // The first frame: VATs' fonts and theme into the context, then the editor itself.
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            io.ConfigWindowsMoveFromTitleBarOnly = true;
            vats::load_fonts(sHost->paths().assets.c_str());
            sApp = std::make_unique<vats::App>(*sHost);
            std::string err;
            if (!sApp->init(1.f, err))  // ImGui's units are the viewer's scaled UI units already
            {
                LL_WARNS("VATsEditor") << "the editor could not start: " << err << LL_ENDL;
                sApp.reset();
                sFailed = true;
                return;
            }
            LL_INFOS("VATsEditor") << "editor open, user data in " << sHost->paths().user << LL_ENDL;
        }
        if (sApp)
            sHost->beforeFrame();
    }

    void editorDraw()
    {
        if (!sApp || sQuit)
            return;
        sQuit = !sApp->frame();
        sHost->afterFrame();
    }

    void closeNow()
    {
        if (sApp)
        {
            sApp->shutdown();
            sApp.reset();
        }
        sHost->close();
        FSVATsImGui::removeClient(CLIENT);
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
        sOpen = sQuit = sClosing = sFailed = false;
        gSavedSettings.setBOOL("VATsEditor", false);
        LL_INFOS("VATsEditor") << "editor closed" << LL_ENDL;
    }
}

void FSVATsEditor::update(bool want_open)
{
    if (!sOpen)
    {
        if (want_open && !LLApp::isExiting())
        {
            if (!sHost)
                sHost = std::make_unique<ViewerHost>();
            FSVATsImGui::setClient(CLIENT, editorDraw, editorBefore);
            sOpen = true;
        }
        return;
    }
    // The viewer is quitting or logging out: no prompt, unsaved work is kept as an autosave (UI-9).
    if (sApp && (LLApp::isExiting() || LLAppViewer::instance()->quitRequested() || LLAppViewer::instance()->logoutRequestSent()))
    {
        sApp->quit_unattended();
        return closeNow();
    }
    if (sQuit || sFailed || (!want_open && !sApp))
        return closeNow();
    if (!want_open && !sClosing)
    {
        // Unticked: the editor asks about unsaved changes and closes once the UI quits (Cancel keeps it open).
        sApp->request_quit();
        sClosing = true;
    }
    else if (want_open)
    {
        sClosing = false;
    }
}

bool FSVATsEditor::ownsWorld()
{
    return sOpen;
}
