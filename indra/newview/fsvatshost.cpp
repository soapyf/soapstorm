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
#include "llchannelmanager.h"
#include "llchiclet.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llmodaldialog.h"
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
#include "llrootview.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llweb.h"
#include "llworld.h"
#include "lldatapacker.h"
#include "llenvironment.h"
#include "llimagepng.h"
#include "llsettingsvo.h"
#include "llkeyframemotion.h"
#include "pipeline.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <memory>
#include <map>
#include <set>

namespace
{
    using vats::Vec3;

    Vec3 toVATs(const LLVector3& v) { return { v.mV[VX], v.mV[VY], v.mV[VZ] }; }
    LLVector3 toLL(const Vec3& v) { return LLVector3(F32(v.x), F32(v.y), F32(v.z)); }

    bool sameJoints(const std::vector<VATsClipMotion::Joint>& a, const std::vector<VATsClipMotion::Joint>& b)
    {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const auto& x, const auto& y)
                          { return x.name == y.name && x.position == y.position && x.base == y.base; });
    }

    constexpr const char* AUTOPILOT = "VATsEditor";  // the behaviour name of the editor's return autopilot
    constexpr F32 RETURN_DISTANCE = 0.5f;              // metres the region may move a flying avatar before it is flown back
    constexpr U32 MOVEMENT = AGENT_CONTROL_AT_POS | AGENT_CONTROL_AT_NEG | AGENT_CONTROL_LEFT_POS | AGENT_CONTROL_LEFT_NEG |
                             AGENT_CONTROL_UP_POS | AGENT_CONTROL_UP_NEG | AGENT_CONTROL_FAST_AT | AGENT_CONTROL_FAST_LEFT |
                             AGENT_CONTROL_FAST_UP | AGENT_CONTROL_NUDGE_AT_POS | AGENT_CONTROL_NUDGE_AT_NEG |
                             AGENT_CONTROL_NUDGE_LEFT_POS | AGENT_CONTROL_NUDGE_LEFT_NEG | AGENT_CONTROL_NUDGE_UP_POS |
                             AGENT_CONTROL_NUDGE_UP_NEG | AGENT_CONTROL_TURN_LEFT | AGENT_CONTROL_TURN_RIGHT;

    void tip(const std::string& text, const char* kind = "SystemMessageTip")
    {
        LLNotificationsUtil::add(kind, LLSD().with("MESSAGE", text));
    }

    // What of the viewer's own UI stays over the editor while its UI is hidden (spec 09 U4b), in one place. Kept:
    // what needs an answer or brings people's words; the rest (toolbars, bars, every other floater) is hidden.
    bool shownOverEditor(LLFloater* floater)
    {
        // Modal dialogs: the alerts that need an answer (the upload price, disconnect, quit) and every toast
        // (notifications, script dialogs, teleport, friendship and permission offers, group notices).
        if (dynamic_cast<const LLModalDialog*>(floater))
            return true;
        static const std::set<std::string> names = {
            "fs_im_container",           // Conversations: nearby chat and IMs (the editor's Chat pane)
            "fs_nearby_chat",            // nearby chat, torn off
            "fs_impanel",                // an IM, torn off
            "script_floater",            // script dialogs shown as a floater
            "notification_well_window",  // Notifications (Viewer > Notifications)
            "im_well_window",            // the IM well
        };
        return names.count(floater->getInstanceName()) > 0;
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
    class ViewerHost final : public vats::ui::Host, public vats::ui::Host::HostUi
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
            const std::string user = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "vats");
            mPaths.user = user + delim;
            LLFile::mkdir(mPaths.user);
            mPaths.settings = mPaths.user + "settings.json";
        }

        const vats::ui::Paths& paths() const override { return mPaths; }

        // The world is the view. The UI sends only what the world lacks (other actors' bodies, props; spec 09 U5), kept as
        // agent-frame triangles for drawScene. Thumbnails: none, the UI shows its plain icons.
        bool scene_begin(vats::ui::SceneTarget target, int, int, const vats::Camera&, const vats::SceneColours&,
                         const vats::Mat4*) override;
        void scene_ground(const Vec3&) override {}
        void scene_triangles(const std::vector<vats::Vertex>& verts, const std::vector<std::uint32_t>& indices, bool, float,
                             bool translucent) override;
        ImTextureID scene_end() override { mSceneOpen = false; return ImTextureID{}; }
        bool save_thumbnail_png(const std::string&) override { return false; }
        ImTextureID load_texture(const std::string& png) override;
        void free_texture(ImTextureID texture) override;
        void set_light(const vats::LightPreset* preset) override;

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
        void set_view_frame(const vats::Xform& edited_in_yours) override;
        std::vector<std::string> joint_overrides() const override;
        void setSkeleton(const vats::Skeleton* skel) { mSkel = skel; }  // before the first frame, so body_shape is ready
        // Spec 09 §5a: the face-positions check without uploading; a line for the log and the status bar.
        std::string faceCheck(const vats::App::FaceCheck& fc);

        bool skin_colours(vats::HostColours& out) const override;
        std::string host_name() const override { return LLVersionInfo::instance().getChannel(); }

        bool can_upload() const override { return true; }

        // The viewer's own UI beside the editor (spec 09 U4b): hidden but for shownOverEditor, conversations in
        // the Chat pane.
        vats::ui::Host::HostUi* host_ui() override { return this; }
        const char* pane_title() const override { return "Chat"; }
        void place_pane(bool shown, ImVec2 min, ImVec2 max) override;
        void place_view(ImVec2 min, ImVec2 max) override;
        int unread_notices() const override;
        void toggle_notices() override { LLFloaterReg::toggleInstanceOrBringToFront("notification_well_window"); }
        const char* reveal_label() const override { return "Show Firestorm UI"; }
        const char* reveal_shortcut() const override { return "Alt+Shift+U"; }
        bool revealed() const override { return mRevealed; }
        void reveal(bool on) override;
        void upload_anim(const std::vector<std::uint8_t>& bytes, const std::string& name,
                         std::function<void(const std::string&)> done) override;

        void drawScene();    // with the world layer, before ImGui's draw: the triangles the UI sent this frame
        void releaseGL();    // the GL context is going
        void beforeFrame(bool reset_joints, bool show_others);  // outside the ImGui frame: the world's frame, the worn body, the camera
        bool hidingOthers() const { return mHidingOthers; }
        // Build 17: while the viewer's UI is hidden, toasts and notifications stay inside the editor's view (the world area
        // between its docked panels), in the viewer's scaled screen coordinates; false: the viewer's own places.
        bool toastArea(LLRect& out) const;
        bool hidesWorldTips() const { return holdsAvatar() && !mRevealed; }
        bool holdsAvatar() const { return mIsolatedOn && isAgentAvatarValid() && mIsolatedOn == gAgentAvatarp.get(); }
        void afterFrame();   // inside it, after the UI's frame: questions, camera edits
        void close();        // the editor is going: stop driving the avatar and the sound, drop its callbacks
        U32 filterControls(U32 flags);
        void hideChrome();   // the viewer's UI hidden but for shownOverEditor
        void showChrome();   // and back exactly as it was
        void releasePane();  // the conversations floater back where it was
        void updateToasts(); // the toast channels laid out again when their area changes (or goes)
        void hideOthers(bool hide);  // every other avatar hidden on this screen, or back as before
        bool regionKeepsStanding() const { return mRegionKeepsStanding; }

    private:
        LLJoint* avatarJoint(int node);
        void isolate(bool reset_joints);  // the avatar belongs to the editor: sit it down once, stop every other motion
        void watchRegion(LLVOAvatarSelf* avatar);  // what the region does meanwhile: stands, teleports, drift
        void restore();      // everything back as it was: motions still wanted, and stand up if we sat
        void updateShape();
        void checkFrame();   // logs the drawn body against the editor's frame (spec 09 §5a)
        void stopMotion();
        void drawQuestion();
        void pushCamera();
        void startSound();
        void stopSound();
        void restoreSky();   // the local environment back as it was before the first Light preset
        // The UI's space is the edited actor's; mView places it in your actor's (the worn avatar's), whose origin is at
        // the feet. toAgentYours maps your actor's own space (the pose the avatar is driven with).
        LLVector3 toAgentYours(const Vec3& p) const { return toLL(p - pelvisRest()) * mRot + mPos; }
        LLVector3 toAgent(const Vec3& p) const { return toAgentYours(mView.apply(p)); }
        Vec3 fromAgent(const LLVector3& a) const { return mView.inverse().apply(toVATs((a - mPos) * ~mRot) + pelvisRest()); }
        Vec3 dirFromAgent(const LLVector3& d) const { return mView.rot.conj().rotate(toVATs(d * ~mRot)); }
        void syncCamera();  // the UI's camera and projection from LLViewerCamera, in the UI's space
        // VATs' space has its origin at the feet (the pelvis rests 1.067 m up); the viewer's mRoot sits where
        // the pelvis rests.
        Vec3 pelvisRest() const { return mSkel && mSkel->size() ? (*mSkel)[0].pos : Vec3{ 0, 0, 1.067 }; }

        vats::ui::Paths mPaths;
        vats::Camera mCamera, mSynced;  // mSynced: as read from the viewer this frame, to spot the UI's edits
        vats::Mat4 mViewProj;
        LLVector3 mPos;                  // VATs' space in the agent frame: the avatar's mRoot
        LLQuaternion mRot;
        vats::Xform mView;               // the edited actor in your actor's space (identity: you edit your own)
        LLRect mViewRect;                // the editor's view, scaled screen coordinates (place_view)
        LLRect mToastRect;               // the toasts' area as last laid out; empty: the viewer's own

        const vats::Skeleton* mSkel = nullptr;
        vats::Pose mPose;               // what VATsClipMotion::sEditor shows
        std::vector<VATsClipMotion::Joint> mJoints;
        std::vector<bool> mDrivesPos;    // by node
        // Each position-driven joint's own position before the editor drove it (the worn avatar's: sliders and
        // mesh joint offsets). The editor's offsets are added to it, and it is put back when the editor lets go.
        std::map<std::string, LLVector3> mBase;
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
        // U4b: the region standing up an avatar the editor sat (re-sit once, then close), flying, the camera.
        bool mSatSeen = false;           // the sit the editor asked for has arrived
        int mRegionStands = 0;
        bool mRegionKeepsStanding = false;
        F64 mResitAt = -1;               // when to sit down again (seconds, LLTimer), -1 = not due
        const LLViewerRegion* mRegion = nullptr;
        bool mFlew = false;              // flying as the editor opened: it hovers instead of sitting
        bool mFrameOnOpen = false;       // frame the avatar from the front on the next frame (once per open)
        F64 mReturnAt = 0;               // the earliest time for the next return autopilot
        F64 mToldAt = 0;                 // the earliest time for the next "Close the editor to stand up"
        F64 mCheckAt = -1;               // checkFrame: the next log line (-1: start over), how many so far, sitting then
        int mChecks = 0;
        bool mCheckSitting = false;
        vats::Quat mCheckFinger;
        // The viewer's UI while the editor is open (U4b).
        bool mChromeHidden = false;      // hidden by the editor
        bool mUiWasVisible = true;       // the viewer's UI was showing before (its own Show UI toggle)
        bool mRevealed = false;          // Show Firestorm UI is on
        bool mHidingOthers = false;      // other avatars hidden (spec 09 U5), and Render Only Friends as it was before
        bool mFriendsOnlyWas = false;
        std::vector<LLHandle<LLFloater>> mHiddenFloaters;  // floaters hidden since, shown again on close
        struct Pane
        {
            LLHandle<LLFloater> floater;
            LLRect rect;
            bool visible = false, drag = true, close = true, resize = true, minimize = true, minimized = false, docked = false;
        } mPane;                         // the conversations floater as it was before it filled the Chat pane

        // The world layer's triangles (spec 09 U5): x y z, normal, rgba per vertex, in the agent frame; opaque, then translucent.
        struct SceneBatch
        {
            std::vector<F32> verts;
            std::vector<U32> indices;
        };
        SceneBatch mScene[2];
        bool mSceneOpen = false;
        U32 mProgram = 0, mVao = 0, mVbo = 0, mEbo = 0;
        S32 mMvpLoc = -1, mLightLoc = -1, mRectLoc = -1, mUseDepthLoc = -1, mDepthLoc = -1;
        bool mProgramFailed = false;
        std::set<GLuint> mTextures;      // the help's pictures (load_texture), until freed or the context goes
        // The local environment before the first Light preset (08 LT-1), put back by restoreSky.
        bool mSkySaved = false, mHadLocal = false;
        LLEnvironment::EnvSelection_t mPrevSelection = LLEnvironment::ENV_LOCAL;
        LLSettingsDay::ptr_t mPrevDay;
        LLSettingsDay::Seconds mPrevDayLength, mPrevDayOffset;
        LLEnvironment::fixedEnvironment_t mPrevFixed;

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
            if (drives_pos[i])
            {
                // The worn position: a mesh's joint offset when one is active (attachments can rez after the
                // editor opens), else the joint's position before the editor first drove it.
                LLVector3 mesh_pos;
                LLUUID mesh_id;
                // The pelvis rests on mRoot (SL-315; the viewer's pelvis_fix motion holds it at zero). What it holds
                // after a skeleton reset (avatar_skeleton.xml's 1.067 m) is not its own position: taking that as the
                // base drew the body raised above the frame the editor draws its bones in.
                if (i == 0)
                    mBase[n.name] = LLVector3::zero;
                else if (avatarJoint(i)->hasAttachmentPosOverride(mesh_pos, mesh_id))
                    mBase[n.name] = mesh_pos;
                else if (!mBase.count(n.name))
                    mBase[n.name] = avatarJoint(i)->getPosition();
            }
            joints.push_back({ n.name, drives_pos[i], drives_pos[i] ? mBase[n.name] : LLVector3::zero });
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
            // A stopped motion leaves its positions behind: the joints it moved go back to their own.
            for (const VATsClipMotion::Joint& j : mJoints)
                if (j.position)
                    if (LLJoint* joint = gAgentAvatarp->getJoint(j.name))
                        joint->setPosition(j.base);
        }
        mMotionID.setNull();
        mMotionOn = nullptr;
        mJoints.clear();
    }

    // Spec 09 §0e: the joints whose position a worn mesh overrides, by skeleton name (names only).
    std::vector<std::string> ViewerHost::joint_overrides() const
    {
        std::vector<std::string> out;
        if (!mSkel || !isAgentAvatarValid())
            return out;
        ViewerHost* self = const_cast<ViewerHost*>(this);  // ponytail: the joint cache is the only state touched
        LLVector3 pos;
        LLUUID mesh_id;
        for (int i = 0; i < mSkel->size(); ++i)
            if (LLJoint* joint = self->avatarJoint(i); joint && joint->hasAttachmentPosOverride(pos, mesh_id))
                out.push_back((*mSkel)[i].name);
        return out;
    }

    // Spec 09 §5a, "Face positions check without uploading", as one step: the exact upload bytes are decoded by the
    // viewer's own LLKeyframeMotion (a local instance, never started or signalled, no asset request) and sampled at
    // each frame; every face joint's position relative to mHead is compared with the editor's preview of the same
    // frame (rotations rest x pose, positions the joint's own plus the pose's offset, as VATsClipMotion sets them).
    // The same with Bake shape SL Default is the negative control: on a mesh head with its own face positions it must
    // be centimetres off, or the check isn't measuring the bytes.
    std::string ViewerHost::faceCheck(const vats::App::FaceCheck& fc)
    {
        if (!mSkel || !isAgentAvatarValid() || fc.poses.size() != fc.frames.size())
            return "Face positions check: log in with the editor open";
        LLVOAvatarSelf* avatar = gAgentAvatarp.get();
        const vats::Skeleton& skel = *mSkel;
        const int head = skel.find("mHead");
        std::vector<std::vector<int>> chains;  // mHead's descendants down to each face joint
        for (int i = 0; i < skel.size(); ++i)
        {
            if (skel[i].category != vats::Category::Face || !avatarJoint(i))
                continue;
            std::vector<int> chain;
            int k = i;
            for (; k >= 0 && k != head; k = skel[k].parent)
                chain.insert(chain.begin(), k);
            if (k == head)
                chains.push_back(std::move(chain));
        }
        auto base = [&](int n) {
            const auto it = mBase.find(skel[n].name);  // a joint the editor drives: its own position before
            return it != mBase.end() ? it->second : avatarJoint(n)->getPosition();
        };
        auto quat = [](const vats::Quat& q) { return LLQuaternion(F32(q.x), F32(q.y), F32(q.z), F32(q.w)); };
        auto measure = [&](const std::vector<std::uint8_t>& bytes, F32& worst, std::string& where) {
            worst = 0.f;
            const LLUUID id = LLUUID::generateNewID();
            auto motion = std::make_unique<LLKeyframeMotion>(id);
            motion->onInitialize(avatar);  // no data yet: it only takes the avatar (nothing is requested until a second call)
            std::vector<U8> copy(bytes.begin(), bytes.end());
            LLDataPackerBinaryBuffer dp(copy.data(), S32(copy.size()));
            void* hand_pose = avatar->getAnimationData("Hand Pose");  // onUpdate sets these; put back below
            void* hand_priority = avatar->getAnimationData("Hand Pose Priority");
            const bool ok = motion->deserialize(dp, id, false);
            std::vector<U8> mask(LL_CHARACTER_MAX_ANIMATED_JOINTS, 0);
            for (size_t s = 0; ok && s < fc.frames.size(); ++s)
            {
                motion->onUpdate(F32(fc.frames[s] / fc.fps), mask.data());
                const vats::Pose& pose = fc.poses[s];
                for (const std::vector<int>& chain : chains)
                {
                    LLVector3 pa, pb;
                    LLQuaternion ra, rb;
                    for (int n : chain)
                    {
                        const LLQuaternion la = quat((skel[n].rest * pose.rot[n]).normalized());
                        const LLVector3 lpa = base(n) + toLL(pose.offset[n]);
                        LLJointState* st = motion->getPose()->findJointState(avatarJoint(n));
                        const bool rot = st && (st->getUsage() & LLJointState::ROT), pos = st && (st->getUsage() & LLJointState::POS);
                        const LLQuaternion lb = rot ? st->getRotation() : quat(skel[n].rest.normalized());
                        const LLVector3 lpb = pos ? st->getPosition() : base(n);
                        pa += lpa * ra, ra = la * ra;
                        pb += lpb * rb, rb = lb * rb;
                    }
                    if (const F32 d = (pa - pb).length(); d > worst)
                    {
                        worst = d;
                        where = skel[chain.back()].name + " at frame " + std::to_string(int(fc.frames[s]));
                    }
                }
            }
            if (hand_pose)
                avatar->setAnimationData("Hand Pose", hand_pose);
            else
                avatar->removeAnimationData("Hand Pose");
            if (hand_priority)
                avatar->setAnimationData("Hand Pose Priority", hand_priority);
            else
                avatar->removeAnimationData("Hand Pose Priority");
            motion.reset();
            LLKeyframeDataCache::removeKeyframeData(id);
            return ok;
        };
        F32 upload = 0.f, control = 0.f;
        std::string at_upload, at_control;
        if (!measure(fc.bytes, upload, at_upload) || !measure(fc.control, control, at_control))
            return "Face positions check: the viewer could not read the exported bytes";
        const bool pass = upload <= 0.001f;
        const bool control_differs = control >= 0.01f;
        return llformat("Face positions check: %s. The upload bytes play within %.2f mm of the preview (%d face joints, "
                        "%d frames%s%s). SL Default control: %.1f cm off%s%s.",
                        pass ? "PASS" : "FAIL", upload * 1000.f, int(chains.size()), int(fc.frames.size()),
                        pass ? "" : "; worst ", pass ? "" : at_upload.c_str(), control * 100.f,
                        control_differs ? " at " : " (no worn face positions to tell apart: inconclusive)",
                        control_differs ? at_control.c_str() : "");
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
            const auto base = mBase.find(skel[i].name);  // a joint the editor drives: its own position
            mShape.offset[i] = toVATs(base != mBase.end() ? base->second : joint->getPosition()) - skel[i].pos;
        }
        mHaveShape = true;
    }

    // Spec 09 U4: while the editor is open the avatar is at its whims. Once, it sits down on the ground (the
    // viewer's own Sit Down: AgentUpdate's sit-on-ground flag), so it cannot be walked or bumped; then every frame
    // every other motion on it is stopped locally (AO, server and scripted animations, stands, walks, look-at, eye
    // and head motion, breathing, expressions), straight on the motion controller, so nothing is sent to the
    // region. What the region starts meanwhile is stopped the same way when it arrives. U4b: flying, it stays in
    // the air instead (no movement input reaches it, see filterControls); it is drawn pinned where it was
    // (VATsClipMotion's pin); the camera stops following it; optionally the skeleton is reset locally.
    void ViewerHost::isolate(bool reset_joints)
    {
        if (!isAgentAvatarValid() || LLStartUp::getStartupState() < STATE_STARTED)
            return;
        LLVOAvatarSelf* avatar = gAgentAvatarp.get();
        const bool first = mIsolatedOn != avatar;
        if (first)
        {
            mIsolatedOn = avatar;
            mStopped.clear();
            mWeSat = mSatSeen = mRegionKeepsStanding = false;
            mRegionStands = 0;
            mResitAt = -1;
            mRegion = gAgent.getRegion();
            mFlew = gAgent.getFlying();
            VATsClipMotion::sEditor.pinned = false;
            mCheckAt = -1;         // checkFrame logs the frame again
            mFrameOnOpen = true;  // beforeFrame frames the avatar; the camera then stays put (no longer following it)
            if (!mRevealed)
                hideChrome();
            // Not while flying (the viewer's Sit Down is disabled then) or already seated; RLVa may refuse.
            if (!avatar->isSitting() && !mFlew && !avatar->isEditingAppearance())
            {
                gAgent.sitDown();
                mWeSat = true;
                LL_INFOS("VATsEditor") << "sitting the avatar down on the ground while the editor is open" << LL_ENDL;
            }
            else if (mFlew)
            {
                LL_INFOS("VATsEditor") << "flying: the avatar stays in the air, hovering, while the editor is open" << LL_ENDL;
            }
        }
        LLMotionController& controller = avatar->getMotionController();
        std::vector<LLUUID> others;
        for (LLMotion* motion : controller.getActiveMotions())
            // The ground sit stays: the viewer takes its motion as the sign the avatar sits (without it,
            // LLVOAvatar::updateCharacter gets the avatar off the ground locally). The editor's pose covers it.
            if (motion && motion->getID() != mMotionID && motion->getID() != ANIM_AGENT_SIT_GROUND_CONSTRAINED)
                others.push_back(motion->getID());
        for (const LLUUID& id : others)
        {
            controller.stopMotionLocally(id, true);
            mStopped.insert(id);
        }
        if (first && reset_joints)
        {
            // As the viewer's Reset Skeleton does on your own avatar (LLVOAvatar::resetSkeleton(false)), locally:
            // joint positions left by stopped animations go; mesh joint offsets are cleared and put back.
            stopMotion();  // restarted by the next drive_avatar
            avatar->resetSkeleton(false);
            // resetSkeleton rebuilds the pelvis at avatar_skeleton.xml's 1.067 m and leaves pelvis_fix (stopped
            // above) to put it back on mRoot; do that here, as LLVOAvatar::clearAttachmentOverrides does.
            if (avatar->mPelvisp)
                avatar->mPelvisp->setPosition(LLVector3::zero);
            mCacheFor = nullptr;
            mBase.clear();  // taken again from the reset skeleton
            LL_INFOS("VATsEditor") << "skeleton reset locally as the editor opens" << LL_ENDL;
        }
        VATsClipMotion::sEditor.pin = !avatar->getParent();  // seated on an object, it goes where the object goes
        if (mChromeHidden && gFloaterView)
        {
            // A floater that opens meanwhile (a script's map, a new window) stays hidden unless allowed.
            const LLView::child_list_t children = *gFloaterView->getChildList();
            for (LLView* view : children)
            {
                LLFloater* floater = dynamic_cast<LLFloater*>(view);
                if (floater && floater->getVisible() && !shownOverEditor(floater) && floater != mPane.floater.get())
                {
                    floater->setVisible(false);
                    mHiddenFloaters.push_back(floater->getHandle());
                }
            }
        }
        watchRegion(avatar);
    }

    // Spec 09 U4b. The region can still move the avatar: a script or the region stands it up, a teleport takes it
    // elsewhere, a push moves it while flying. Only standard viewer actions answer: Sit Down again (once; a second
    // stand closes the editor), and the viewer's autopilot to fly back. Nothing else is sent.
    void ViewerHost::watchRegion(LLVOAvatarSelf* avatar)
    {
        VATsClipMotion::Playback& pb = VATsClipMotion::sEditor;
        const F64 now = LLTimer::getTotalSeconds();
        if (gAgent.getAutoPilot() && gAgent.getAutoPilotBehaviorName() != AUTOPILOT)
            gAgent.stopAutoPilot(true);  // a walk the viewer's UI started (the minimap's double-click...)
        // A teleport or region crossing: a new place, so a new pin and a sit there that doesn't count as a stand.
        if (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE || gAgent.getRegion() != mRegion)
        {
            mRegion = gAgent.getRegion();
            pb.pinned = false;
            mSatSeen = false;
            if (mWeSat)
                mResitAt = now + 2.0;
            return;
        }
        if (mWeSat)
        {
            if (avatar->isSitting())
            {
                mSatSeen = true;
                mResitAt = -1;
            }
            else if (mSatSeen)
            {
                mSatSeen = false;
                ++mRegionStands;
                LL_INFOS("VATsEditor") << "the region stood the avatar up while the editor is open (" << mRegionStands
                                        << (mRegionStands == 1 ? "st time: sitting down again)" : "nd time: closing the editor)") << LL_ENDL;
                if (mRegionStands >= 2)
                    mRegionKeepsStanding = true;
                else
                    mResitAt = now + 1.0;
            }
            if (mResitAt >= 0 && now >= mResitAt)
            {
                mResitAt = -1;
                if (!avatar->isSitting() && !gAgent.getFlying() && !avatar->isEditingAppearance())
                {
                    gAgent.sitDown();
                    LL_INFOS("VATsEditor") << "sitting the avatar down on the ground again" << LL_ENDL;
                }
                else
                {
                    LL_INFOS("VATsEditor") << "not sitting down again: seated, flying or editing appearance" << LL_ENDL;
                }
            }
        }
        else if (mFlew && gAgent.getFlying() && pb.pin && pb.pinned && !gAgent.getAutoPilot() && now >= mReturnAt)
        {
            const F32 drift = (pb.root_at_update - pb.pin_pos).length();
            if (drift > RETURN_DISTANCE)
            {
                mReturnAt = now + 5.0;  // one try at a time; a failed one gives up after the autopilot's own timeout
                LL_INFOS("VATsEditor") << "the region moved the flying avatar " << drift << " m; flying it back with the viewer's autopilot" << LL_ENDL;
                gAgent.startAutoPilotGlobal(gAgent.getPosGlobalFromAgent(pb.pin_pos), AUTOPILOT, &pb.pin_rot, nullptr, nullptr, 0.25f);
            }
        }
    }

    U32 ViewerHost::filterControls(U32 flags)
    {
        if (!mIsolatedOn || !isAgentAvatarValid() || mIsolatedOn != gAgentAvatarp.get())
            return flags;
        if (!(gAgent.getAutoPilot() && gAgent.getAutoPilotBehaviorName() == AUTOPILOT))
            flags &= ~MOVEMENT;
        if ((flags & AGENT_CONTROL_STAND_UP) && mWeSat)
        {
            flags &= ~AGENT_CONTROL_STAND_UP;
            const F64 now = LLTimer::getTotalSeconds();
            if (gAgentAvatarp->isSitting() && now >= mToldAt)
            {
                mToldAt = now + 3.0;
                tip("Close the editor to stand up");
                LL_INFOS("VATsEditor") << "Stand Up refused while the editor is open" << LL_ENDL;
            }
        }
        return flags;
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
        if (gAgent.getAutoPilot() && gAgent.getAutoPilotBehaviorName() == AUTOPILOT)
            gAgent.stopAutoPilot();
        const bool leaving = LLApp::isExiting() || LLAppViewer::instance()->quitRequested() || LLAppViewer::instance()->logoutRequestSent();
        // Sitting on the ground as the region has it (its ground-sit animation), whatever the local flag says.
        const bool on_ground = avatar->isSitting() || avatar->mSignaledAnimations.count(ANIM_AGENT_SIT_GROUND_CONSTRAINED);
        const bool stand = mWeSat && !leaving && on_ground && !avatar->getParent();
        LL_INFOS("VATsEditor") << "avatar handed back: " << restarted << " of " << mStopped.size()
                                << " stopped motions restarted" << (stand ? ", standing up" : "") << LL_ENDL;
        mIsolatedOn = nullptr;  // first: filterControls lets the Stand Up through
        mStopped.clear();
        mWeSat = mRegionKeepsStanding = false;
        if (stand)
            gAgent.standUp();  // the viewer's own Stand Up
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

    void ViewerHost::beforeFrame(bool reset_joints, bool show_others)
    {
        static LLCachedControl<bool> show_ui(gSavedSettings, "VATsShowViewerUI", false);
        if (show_ui != mRevealed)
            reveal(show_ui);
        isolate(reset_joints);
        hideOthers(holdsAvatar() && !show_others);
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        if (isAgentAvatarValid() && gAgentAvatarp->getRootJoint())
        {
            LLJoint* root = gAgentAvatarp->getRootJoint();
            mPos = root->getWorldPosition();
            mRot = root->getWorldRotation();
            const VATsClipMotion::Playback& pb = VATsClipMotion::sEditor;
            if (pb.pin && pb.pinned)  // drawn where the avatar is drawn (the pin), with any later root offset
            {
                mPos = pb.pin_pos + (mPos - pb.root_at_update);
                mRot = pb.pin_rot;
            }
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
        checkFrame();
        syncCamera();
        if (std::exchange(mFrameOnOpen, false))
        {
            // As Frame All does, from the front; pushCamera moves the viewer's camera there, unfollowed, as after an
            // Alt+click, so a push on the region doesn't move the view.
            mCamera.target = Vec3{ 0, 0, 1.0 };
            mCamera.distance = vats::Camera::kDefaultDistance;
            mCamera.yaw = 0;
            mCamera.pitch = 0.1;
        }

        if (mPlayAt >= 0 && LLTimer::getTotalSeconds() >= mPlayAt)
            startSound();
    }

    // The viewer's camera is the view: the UI's camera follows it every frame (in VATs' space), and the projection is
    // LLViewerCamera's own, so markers and gizmos sit on the world.
    void ViewerHost::syncCamera()
    {
        LLViewerCamera* cam = LLViewerCamera::getInstance();
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
    }

    // Build 17: editing another actor than yours, the UI's space is that actor's. The camera is read again in the new
    // space at once, so the change is not taken for a camera edit of the UI's (pushCamera).
    void ViewerHost::set_view_frame(const vats::Xform& edited_in_yours)
    {
        const bool same = (edited_in_yours.pos - mView.pos).length() < 1e-9 && std::fabs(edited_in_yours.rot.dot(mView.rot)) > 1 - 1e-12;
        if (same)
            return;
        mView = edited_in_yours;
        syncCamera();
    }

    // Spec 09 §5a: the body as drawn against the editor's frame, logged 1, 3 and 6 s after the editor takes the avatar
    // and again after each change between sitting and standing. The editor's pose and shape this frame are the ones
    // the avatar was drawn with (VATsClipMotion updates before the render), so each pair should agree to a millimetre.
    void ViewerHost::checkFrame()
    {
        if (!mIsolatedOn || !isAgentAvatarValid() || !mSkel || mPose.rot.size() != (size_t)mSkel->size())
            return;
        LLVOAvatarSelf* avatar = gAgentAvatarp.get();
        const F64 now = LLTimer::getTotalSeconds();
        const bool sitting = avatar->isSitting();
        if (mCheckAt < 0 || sitting != mCheckSitting)
        {
            mCheckSitting = sitting;
            mChecks = 0;
            mCheckAt = now + 1.0;
        }
        // A hand pose applied later (Inventory > Starter poses) logs once more, to show the finger joints follow.
        const int finger = mSkel->find("mHandIndex2Left");
        if (finger >= 0 && mChecks >= 3 && std::fabs(mPose.rot[finger].dot(mCheckFinger)) < 0.999)
            mChecks = 2, mCheckAt = now + 0.5;
        if (mChecks >= 3 || now < mCheckAt)
            return;
        if (finger >= 0)
            mCheckFinger = mPose.rot[finger];
        mCheckAt = now + (++mChecks == 1 ? 2.0 : 3.0);
        const std::vector<vats::Xform> g = mSkel->global_pose(mPose, mHaveShape ? &mShape : nullptr);
        std::string line;
        for (const char* name : { "mPelvis", "mFootLeft", "mHead", "mHandIndex2Left" })
        {
            const int node = mSkel->find(name);
            LLJoint* joint = node >= 0 ? avatarJoint(node) : nullptr;
            if (!joint)
                continue;
            const LLVector3 drawn = joint->getWorldPosition(), editor = toAgentYours(g[node].pos);
            line += llformat(" %s drawn <%.3f %.3f %.3f> editor <%.3f %.3f %.3f> off %.3f m;", name, drawn.mV[VX], drawn.mV[VY],
                             drawn.mV[VZ], editor.mV[VX], editor.mV[VY], editor.mV[VZ], (drawn - editor).length());
        }
        LLJoint* foot = avatar->getJoint("mFootLeft");
        const F32 ground = foot ? LLWorld::getInstance()->resolveLandHeightAgent(foot->getWorldPosition()) : 0.f;
        LL_INFOS("VATsEditor") << "frame check (" << (sitting ? "sitting" : "not sitting") << (avatar->getParent() ? " on an object" : "")
                                << (gAgent.getFlying() ? ", flying" : "") << ", pin " << (VATsClipMotion::sEditor.pinned ? "on" : "off")
                                << "):" << line << " ground under the left foot " << ground << LL_ENDL;
    }

    void ViewerHost::afterFrame()
    {
        drawQuestion();
        pushCamera();
        updateToasts();
    }

    void ViewerHost::place_view(ImVec2 min, ImVec2 max)
    {
        const LLRect world = gViewerWindow->getWorldViewRectScaled();
        mViewRect = LLRect(world.mLeft + S32(std::lround(min.x)), world.mTop - S32(std::lround(min.y)),
                           world.mLeft + S32(std::lround(max.x)), world.mTop - S32(std::lround(max.y)));
    }

    // The editor's view, less a margin, while the viewer's UI is hidden (the editor's panels are where the viewer's toasts
    // would go). Show Firestorm UI or closing gives the channels their own places back.
    bool ViewerHost::toastArea(LLRect& out) const
    {
        constexpr S32 MARGIN = 8;
        if (!mChromeHidden || mViewRect.getWidth() < 2 * MARGIN || mViewRect.getHeight() < 2 * MARGIN)
            return false;
        out = LLRect(mViewRect.mLeft + MARGIN, mViewRect.mTop - MARGIN, mViewRect.mRight - MARGIN, mViewRect.mBottom + MARGIN);
        return true;
    }

    void ViewerHost::updateToasts()
    {
        LLRect area;
        if (!toastArea(area))
            area = LLRect();
        if (area == mToastRect)
            return;
        mToastRect = area;
        for (const auto& elem : LLNotificationsUI::LLChannelManager::getInstance()->getChannelList())
            if (LLNotificationsUI::LLScreenChannelBase* channel = elem.channel.get())
                channel->redrawToasts();
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

    // --- The world layer's triangles (spec 09 U5) ----------------------------------------------------------

    bool ViewerHost::scene_begin(vats::ui::SceneTarget target, int, int, const vats::Camera&, const vats::SceneColours&,
                                 const vats::Mat4*)
    {
        mScene[0].verts.clear(), mScene[0].indices.clear();
        mScene[1].verts.clear(), mScene[1].indices.clear();
        // Only logged in: before that the login page covers the world view, and there is no world depth to test against.
        mSceneOpen = target == vats::ui::SceneTarget::View && LLStartUp::getStartupState() >= STATE_STARTED && !mProgramFailed;
        return mSceneOpen;
    }

    void ViewerHost::scene_triangles(const std::vector<vats::Vertex>& verts, const std::vector<std::uint32_t>& indices, bool,
                                     float, bool translucent)
    {
        if (!mSceneOpen || verts.empty())
            return;
        SceneBatch& b = mScene[translucent ? 1 : 0];
        const U32 base = U32(b.verts.size() / 10);
        b.verts.reserve(b.verts.size() + verts.size() * 10);
        for (const vats::Vertex& v : verts)
        {
            const LLVector3 p = toAgent(Vec3{ v.p[0], v.p[1], v.p[2] });
            const LLVector3 n = toLL(mView.rot.rotate(Vec3{ v.n[0], v.n[1], v.n[2] })) * mRot;
            b.verts.insert(b.verts.end(), { p.mV[VX], p.mV[VY], p.mV[VZ], n.mV[VX], n.mV[VY], n.mV[VZ], v.c[0], v.c[1], v.c[2], v.c[3] });
        }
        if (indices.empty())
            for (U32 i = 0; i < U32(verts.size()); ++i)
                b.indices.push_back(base + i);
        else
            for (U32 i : indices)
                b.indices.push_back(base + i);
    }

    // Plain GL, every state it touches put back (FSVATsImGui checks GL and LLRender's caches around the whole draw on the
    // first frames). The world's own matrices from the end of its render (gGLLast*), the world view as the viewport, and
    // the world's depth (the deferred target's depth texture) sampled per fragment: a fragment behind the world is
    // dropped. Lit by a light at the camera, a little from above. Nothing to draw: nothing is done at all.
    void ViewerHost::drawScene()
    {
        if (mScene[0].indices.empty() && mScene[1].indices.empty())
            return;
        if (!mProgram && !mProgramFailed)
        {
            static const char* vs = "#version 150\n"
                "uniform mat4 u_mvp; in vec3 a_pos; in vec3 a_nrm; in vec4 a_col; out vec3 v_nrm; out vec4 v_col;\n"
                "void main() { gl_Position = u_mvp * vec4(a_pos, 1.0); v_nrm = a_nrm; v_col = a_col; }\n";
            static const char* fs = "#version 150\n"
                "uniform sampler2D u_depth; uniform vec4 u_rect; uniform int u_use_depth; uniform vec3 u_light;\n"
                "in vec3 v_nrm; in vec4 v_col; out vec4 o_col;\n"
                "void main() {\n"
                "  if (u_use_depth != 0) {\n"
                "    ivec2 size = textureSize(u_depth, 0);\n"
                "    ivec2 at = clamp(ivec2((gl_FragCoord.xy - u_rect.xy) / u_rect.zw * vec2(size)), ivec2(0), size - 1);\n"
                "    if (gl_FragCoord.z > texelFetch(u_depth, at, 0).r) discard;\n"
                "  }\n"
                "  float lit = 0.35 + 0.65 * abs(dot(normalize(v_nrm), u_light));\n"
                "  o_col = vec4(v_col.rgb * lit, v_col.a);\n"
                "}\n";
            auto compile = [](GLenum kind, const char* src) {
                const GLuint sh = glCreateShader(kind);
                glShaderSource(sh, 1, &src, nullptr);
                glCompileShader(sh);
                GLint ok = 0;
                glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
                if (!ok)
                {
                    char log[1024] = "";
                    glGetShaderInfoLog(sh, sizeof log, nullptr, log);
                    LL_WARNS("VATsEditor") << "scene shader: " << log << LL_ENDL;
                }
                return sh;
            };
            const GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
            mProgram = glCreateProgram();
            glAttachShader(mProgram, v);
            glAttachShader(mProgram, f);
            glBindAttribLocation(mProgram, 0, "a_pos");
            glBindAttribLocation(mProgram, 1, "a_nrm");
            glBindAttribLocation(mProgram, 2, "a_col");
            glBindFragDataLocation(mProgram, 0, "o_col");
            glLinkProgram(mProgram);
            glDeleteShader(v);
            glDeleteShader(f);
            GLint linked = 0;
            glGetProgramiv(mProgram, GL_LINK_STATUS, &linked);
            if (!linked)
            {
                LL_WARNS("VATsEditor") << "the scene shader did not link: other actors' bodies and props are not drawn" << LL_ENDL;
                glDeleteProgram(mProgram);
                mProgram = 0;
                mProgramFailed = true;
                return;
            }
            mMvpLoc = glGetUniformLocation(mProgram, "u_mvp");
            mLightLoc = glGetUniformLocation(mProgram, "u_light");
            mRectLoc = glGetUniformLocation(mProgram, "u_rect");
            mUseDepthLoc = glGetUniformLocation(mProgram, "u_use_depth");
            mDepthLoc = glGetUniformLocation(mProgram, "u_depth");
            glGenVertexArrays(1, &mVao);
            glGenBuffers(1, &mVbo);
            glGenBuffers(1, &mEbo);
        }

        GLint program = 0, vao = 0, array_buffer = 0, active = 0, texture = 0, viewport[4] = {}, depth_func = 0;
        GLint blend_src_rgb = 0, blend_dst_rgb = 0, blend_src_a = 0, blend_dst_a = 0;
        GLboolean depth_mask = GL_TRUE;
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_a);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_a);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
        const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);

        const LLRect world = gViewerWindow->getWorldViewRectRaw();
        glViewport(world.mLeft, world.mBottom, world.getWidth(), world.getHeight());
        const glm::mat4 mvp = glm::make_mat4(gGLLastProjection) * glm::make_mat4(gGLLastModelView);
        const U32 world_depth = gPipeline.mRT ? gPipeline.mRT->deferredScreen.getDepth() : 0;
        LLVector3 light = -LLViewerCamera::getInstance()->getAtAxis() + LLVector3(0.f, 0.f, 0.5f);
        light.normVec();

        glUseProgram(mProgram);
        glUniformMatrix4fv(mMvpLoc, 1, GL_FALSE, glm::value_ptr(mvp));
        glUniform3f(mLightLoc, light.mV[VX], light.mV[VY], light.mV[VZ]);
        glUniform4f(mRectLoc, (F32)world.mLeft, (F32)world.mBottom, (F32)llmax(world.getWidth(), 1), (F32)llmax(world.getHeight(), 1));
        glUniform1i(mUseDepthLoc, world_depth ? 1 : 0);
        glUniform1i(mDepthLoc, 0);
        glBindTexture(GL_TEXTURE_2D, world_depth);
        glBindVertexArray(mVao);
        glBindBuffer(GL_ARRAY_BUFFER, mVbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mEbo);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 10 * sizeof(F32), (void*)0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 10 * sizeof(F32), (void*)(3 * sizeof(F32)));
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 10 * sizeof(F32), (void*)(6 * sizeof(F32)));
        glDisable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        glClear(GL_DEPTH_BUFFER_BIT);  // the window's own depth holds nothing of the world any more (renderFinalize)
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        for (int pass = 0; pass < 2; ++pass)
        {
            const SceneBatch& b = mScene[pass];
            if (b.indices.empty())
                continue;
            glDepthMask(pass == 0 ? GL_TRUE : GL_FALSE);  // translucent: seen through, writes no depth
            glBufferData(GL_ARRAY_BUFFER, b.verts.size() * sizeof(F32), b.verts.data(), GL_STREAM_DRAW);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, b.indices.size() * sizeof(U32), b.indices.data(), GL_STREAM_DRAW);
            glDrawElements(GL_TRIANGLES, GLsizei(b.indices.size()), GL_UNSIGNED_INT, nullptr);
        }

        glBindVertexArray(vao);  // brings back its element buffer
        glBindBuffer(GL_ARRAY_BUFFER, array_buffer);
        glBindTexture(GL_TEXTURE_2D, texture);
        glActiveTexture(active);
        glUseProgram(program);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glDepthFunc(depth_func);
        glDepthMask(depth_mask);
        glBlendFuncSeparate(blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a);
        (blend ? glEnable : glDisable)(GL_BLEND);
        (depth ? glEnable : glDisable)(GL_DEPTH_TEST);
        (cull ? glEnable : glDisable)(GL_CULL_FACE);
    }

    void ViewerHost::releaseGL()
    {
        // The context is going with them (stopGL); new ones are made on the next draw. The help's pictures go too: the
        // help keeps their stale names until its page changes (drawn blank), and free_texture ignores them.
        mProgram = mVao = mVbo = mEbo = 0;
        mTextures.clear();
    }

    // The help's pictures (spec 09 §0e): a PNG decoded by the viewer's own classes into a plain GL texture, which ImGui's
    // OpenGL 3 backend draws by its name. Small local UI pictures, so no LLViewerTexture and no texture cache.
    ImTextureID ViewerHost::load_texture(const std::string& png)
    {
        LLPointer<LLImagePNG> file = new LLImagePNG;
        LLPointer<LLImageRaw> raw = new LLImageRaw;
        if (!file->load(png) || !file->decode(raw, 0.f) || raw->isBufferInvalid())
            return ImTextureID{};
        const S32 w = raw->getWidth(), h = raw->getHeight(), c = raw->getComponents();
        if (w <= 0 || h <= 0 || c < 1 || c > 4)
            return ImTextureID{};
        // RGBA, top row first: LLImageRaw keeps the bottom row first (LLPngWrapper), and ImGui's UVs start at the top.
        std::vector<U8> rgba(size_t(w) * h * 4);
        const U8* src = raw->getData();
        for (S32 y = 0; y < h; ++y)
        {
            const U8* row = src + size_t(h - 1 - y) * w * c;
            for (S32 x = 0; x < w; ++x)
            {
                const U8* p = row + size_t(x) * c;
                U8* q = &rgba[(size_t(y) * w + x) * 4];
                q[0] = p[0];
                q[1] = c >= 3 ? p[1] : p[0];
                q[2] = c >= 3 ? p[2] : p[0];
                q[3] = c == 4 ? p[3] : c == 2 ? p[1] : 255;
            }
        }
        GLint active = 0, bound = 0, align = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);  // gGL's unit 0 keeps thinking this is bound: put it back
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, align);
        glBindTexture(GL_TEXTURE_2D, bound);
        glActiveTexture(active);
        mTextures.insert(tex);
        return ImTextureID(tex);
    }

    void ViewerHost::free_texture(ImTextureID texture)
    {
        const GLuint tex = GLuint(texture);
        if (mTextures.erase(tex))
            glDeleteTextures(1, &tex);
    }

    // Light menu (08 LT-1): a local sky built from the preset, which only this viewer sees. The key light is in the edited
    // actor's space; the sun (the moon at night) goes where it points, in the region's frame.
    void ViewerHost::set_light(const vats::LightPreset* preset)
    {
        if (!preset)
            return restoreSky();
        LLEnvironment& env = LLEnvironment::instance();
        if (!mSkySaved)
        {
            mSkySaved = true;
            mHadLocal = env.hasEnvironment(LLEnvironment::ENV_LOCAL);
            mPrevSelection = env.getSelectedEnvironment();
            mPrevDay = mHadLocal ? env.getEnvironmentDay(LLEnvironment::ENV_LOCAL) : LLSettingsDay::ptr_t();
            mPrevDayLength = mHadLocal ? env.getEnvironmentDayLength(LLEnvironment::ENV_LOCAL) : LLSettingsDay::Seconds(0);
            mPrevDayOffset = mHadLocal ? env.getEnvironmentDayOffset(LLEnvironment::ENV_LOCAL) : LLSettingsDay::Seconds(0);
            mPrevFixed = mHadLocal ? env.getEnvironmentFixed(LLEnvironment::ENV_LOCAL) : LLEnvironment::fixedEnvironment_t();
        }
        LLVector3 key = toLL(mView.rot.rotate(Vec3{ preset->key[0], preset->key[1], preset->key[2] }));
        key = key * mRot;
        key.normVec();
        LLQuaternion up, down;
        up.shortestArc(LLVector3::x_axis, key);
        down.shortestArc(LLVector3::x_axis, LLVector3(key.mV[VX], key.mV[VY], -std::max(std::fabs(key.mV[VZ]), 0.3f)));
        LLSettingsSky::ptr_t sky = LLSettingsVOSky::buildDefaultSky();
        sky->setSunRotation(preset->night ? down : up);  // at night the sun is under the horizon and the moon lights
        sky->setMoonRotation(preset->night ? up : down);
        sky->setSunlightColor(LLColor3(preset->colour.r, preset->colour.g, preset->colour.b));
        // ponytail: the fill at half the app's strength, a guess until seen in-world; tune here.
        sky->setAmbientColor(LLColor3(preset->ambient.r, preset->ambient.g, preset->ambient.b) * 0.5f);
        if (preset->night)
            sky->setMoonBrightness(1.f);
        sky->setCloudShadow(0.f);
        env.setEnvironment(LLEnvironment::ENV_LOCAL, sky);
        env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
    }

    void ViewerHost::restoreSky()
    {
        if (!mSkySaved)
            return;
        mSkySaved = false;
        LLEnvironment& env = LLEnvironment::instance();
        if (!mHadLocal)
            env.clearEnvironment(LLEnvironment::ENV_LOCAL);
        else if (mPrevDay)
            env.setEnvironment(LLEnvironment::ENV_LOCAL, mPrevDay, mPrevDayLength, mPrevDayOffset);
        else
            env.setEnvironment(LLEnvironment::ENV_LOCAL, mPrevFixed);
        env.setSelectedEnvironment(mPrevSelection, LLEnvironment::TRANSITION_INSTANT);
        mPrevDay.reset();
        mPrevFixed = LLEnvironment::fixedEnvironment_t();
    }

    // --- The viewer's UI beside the editor (spec 09 U4b) -------------------------------------------------

    // The viewer's own hide-UI (its Show UI toggle: toolbars, navigation and status bars, chiclets, floaters),
    // then the allowed floaters shown again.
    void ViewerHost::hideChrome()
    {
        if (mChromeHidden || !gFloaterView || !gViewerWindow)
            return;
        std::vector<LLHandle<LLFloater>> keep;
        for (LLView* view : *gFloaterView->getChildList())
            if (LLFloater* floater = dynamic_cast<LLFloater*>(view); floater && floater->getVisible() && shownOverEditor(floater))
                keep.push_back(floater->getHandle());
        mUiWasVisible = gViewerWindow->getUIVisibility();
        if (mUiWasVisible)
            gViewerWindow->setUIVisibility(false);
        for (const LLHandle<LLFloater>& handle : keep)
            if (LLFloater* floater = handle.get())
                floater->setVisible(true);
        mChromeHidden = true;
        LL_INFOS("VATsEditor") << "viewer UI hidden but for chat, IMs, notifications and alerts" << LL_ENDL;
    }

    void ViewerHost::showChrome()
    {
        if (!mChromeHidden)
            return;
        releasePane();
        if (mUiWasVisible && gViewerWindow)
            gViewerWindow->setUIVisibility(true);  // shows what its hide hid
        for (const LLHandle<LLFloater>& handle : mHiddenFloaters)
            if (LLFloater* floater = handle.get())
                floater->setVisible(true);
        mHiddenFloaters.clear();
        mChromeHidden = false;
        LL_INFOS("VATsEditor") << "viewer UI shown again" << LL_ENDL;
    }

    // Spec 09 U5: the viewer's own Render Only Friends (Developer > Avatar > Character Tests > Render Only Friends,
    // RenderAvatarFriendsOnly, not persisted): an avatar it hides draws nothing, with its attachments, shadow and name
    // tag (LLVOAvatar::isVisible, the avatar draw pool, idleUpdateNameTag). With hidesOtherAvatars, isBuddy answers
    // false, so friends go too; your own avatar and animesh never do. Put back exactly as it was when the editor lets go.
    void ViewerHost::hideOthers(bool hide)
    {
        if (hide == mHidingOthers)
            return;
        if (hide)
        {
            mFriendsOnlyWas = gSavedSettings.getBOOL("RenderAvatarFriendsOnly");
            gSavedSettings.setBOOL("RenderAvatarFriendsOnly", true);
        }
        else
        {
            gSavedSettings.setBOOL("RenderAvatarFriendsOnly", mFriendsOnlyWas);
        }
        mHidingOthers = hide;
        LL_INFOS("VATsEditor") << (hide ? "other avatars hidden on this screen" : "other avatars shown as before") << LL_ENDL;
    }

    // The editor's Viewer menu, the Avatar menu's item or Alt+Shift+U (both through the setting VATsShowViewerUI).
    void ViewerHost::reveal(bool on)
    {
        if (gSavedSettings.getBOOL("VATsShowViewerUI") != on)
            gSavedSettings.setBOOL("VATsShowViewerUI", on);
        if (mRevealed != on)
            LL_INFOS("VATsEditor") << "Show Firestorm UI " << (on ? "on" : "off") << LL_ENDL;
        mRevealed = on;
        if (on)
            showChrome();
        else if (mIsolatedOn)
            hideChrome();
    }

    // The conversations floater fills the Chat pane: moved and sized to it every frame, not draggable, closable or
    // resizable while there, hidden while the pane is (closed, or another tab of its dock).
    void ViewerHost::place_pane(bool shown, ImVec2 min, ImVec2 max)
    {
        if (!mChromeHidden || !gFloaterView)
            return;  // the viewer's full UI is showing: the floater is where the user put it
        LLFloater* chat = mPane.floater.get();
        if (!shown)
        {
            if (chat && chat->getVisible())
                chat->setVisible(false);
            return;
        }
        if (!chat)
        {
            chat = LLFloaterReg::getInstance("fs_im_container");
            if (!chat)
                return;
            mPane.floater = chat->getHandle();
            mPane.rect = chat->getRect();
            mPane.visible = chat->getVisible();
            mPane.drag = chat->getCanDrag();
            mPane.close = chat->isCloseable();
            mPane.resize = chat->isResizable();
            mPane.minimize = chat->isMinimizeable();
            mPane.minimized = chat->isMinimized();
            mPane.docked = chat->isDocked();
            if (mPane.docked)
                chat->setDocked(false);
            if (mPane.minimized)
                chat->setMinimized(false);
            chat->setCanDrag(false);
            chat->setCanClose(false);
            chat->setCanResize(false);
            chat->setCanMinimize(false);
        }
        const LLRect world = gViewerWindow->getWorldViewRectScaled();
        const LLRect screen(world.mLeft + ll_round(min.x), world.mTop - ll_round(min.y), world.mLeft + ll_round(max.x),
                            world.mTop - ll_round(max.y));
        LLRect local;
        gFloaterView->screenRectToLocal(screen, &local);
        if (chat->getRect() != local)
            chat->setShape(local);
        if (!chat->getVisible())
            chat->setVisible(true);
    }

    void ViewerHost::releasePane()
    {
        LLFloater* chat = mPane.floater.get();
        if (chat)
        {
            chat->setCanDrag(mPane.drag);
            chat->setCanClose(mPane.close);
            chat->setCanResize(mPane.resize);
            chat->setCanMinimize(mPane.minimize);
            chat->setShape(mPane.rect);
            chat->setVisible(mPane.visible);
            if (mPane.minimized)
                chat->setMinimized(true);
            if (mPane.docked)
                chat->setDocked(true);
        }
        mPane = Pane();
    }

    int ViewerHost::unread_notices() const
    {
        // The notification well's counter (the chiclet bar hides with the viewer's UI but keeps counting).
        static LLHandle<LLView> chiclet;
        if (!chiclet.get() && gViewerWindow)
            if (LLNotificationChiclet* found = gViewerWindow->getRootView()->findChild<LLNotificationChiclet>("notification_well", true))
                chiclet = found->getHandle();
        LLNotificationChiclet* well = dynamic_cast<LLNotificationChiclet*>(chiclet.get());
        return well ? well->getCounter() : 0;
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
        showChrome();
        mViewRect = LLRect();
        updateToasts();  // the toasts back in the viewer's own places
        mRevealed = false;
        gSavedSettings.setBOOL("VATsShowViewerUI", false);
        hideOthers(false);
        stopMotion();
        mBase.clear();
        restore();
        stopSound();
        restoreSky();
        VATsClipMotion::sEditor = VATsClipMotion::Playback();
        mSkel = nullptr;
        mCacheFor = nullptr;
        mHaveShape = false;
        mView = {};
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
            sHost->setSkeleton(&sApp->skeleton());
        }
        if (sApp)
        {
            sHost->beforeFrame(sApp->viewer_reset_joints(), sApp->viewer_show_others());
            static LLCachedControl<bool> face_check(gSavedSettings, "VATsFaceCheck", false);
            if (face_check)
            {
                // Developer > VATs: Check Face Positions (spec 09 §5a): one run, then the item is ready again.
                gSavedSettings.setBOOL("VATsFaceCheck", false);
                vats::App::FaceCheck fc;
                std::string why;
                const std::string line = sApp->face_check(fc, why) ? sHost->faceCheck(fc) : "Face positions check: " + why;
                LL_INFOS("VATsEditor") << line << LL_ENDL;
                sApp->show_status(line);
                tip(line);
            }
        }
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
    // The region stood the avatar up twice (spec 09 U4b): the work goes to a quicksave the next open reopens.
    if (sApp && sHost->regionKeepsStanding())
    {
        const bool saved = sApp->quit_to_quicksave();
        if (!saved)
            sApp->quit_unattended();
        tip(saved ? "The region kept standing you up, so the editor closed. Your work is saved and reopens next time."
                  : "The region kept standing you up, so the editor closed. Unsaved work is kept as an autosave.",
            "SystemMessage");
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

void FSVATsEditor::drawScene()
{
    if (sHost && sApp)
        sHost->drawScene();
}

void FSVATsEditor::releaseGL()
{
    if (sHost)
        sHost->releaseGL();
}

bool FSVATsEditor::hidesOtherAvatars()
{
    return sHost && sHost->hidingOthers();
}

bool FSVATsEditor::holdsAvatar()
{
    return sHost && sHost->holdsAvatar();
}

bool FSVATsEditor::toastArea(LLRect& out)
{
    return sHost && sApp && sHost->toastArea(out);
}

bool FSVATsEditor::hidesWorldTips()
{
    return sHost && sApp && sHost->hidesWorldTips();
}

bool FSVATsEditor::ownsWorld()
{
    return sOpen;
}

U32 FSVATsEditor::filterControls(U32 flags)
{
    return sHost && (flags & (MOVEMENT | AGENT_CONTROL_STAND_UP)) ? sHost->filterControls(flags) : flags;
}
