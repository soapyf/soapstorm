/**
 * @file fsfloateranimator.h
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

#ifndef FS_FLOATERANIMATOR_H
#define FS_FLOATERANIMATOR_H

#include "llfloater.h"

#include <functional>
#include <memory>

#include "vats/clip.h"
#include "vats/history.h"
#include "vats/rig.h"
#include "vats/skeleton.h"

class FSAnimGraphCtrl;
class FSAnimTimelineCtrl;
class LLScrollListCtrl;

class FSFloaterAnimator : public LLFloater
{
public:
    FSFloaterAnimator(const LLSD& key);
    ~FSFloaterAnimator() override;

    bool postBuild() override;
    void onClose(bool app_quitting) override;
    void draw() override;

    // For motion capture (6h), which records into the open clip.
    bool hasClip() const { return mHaveClip; }
    const vats::Clip& clip() const { return mClip; }
    unsigned generation() const { return mGeneration; }  // changes whenever another clip is opened or created
    int currentFrame() const;
    std::vector<int> selectedNodes() const;  // node indices in the VATs skeleton
    void showFrame(double frame);            // pause and show a frame (the playhead follows a take)
    void mergeTake(const std::string& label, const std::function<void(vats::Clip&)>& change);

    // For VATs Tools (6f, 6g), which edit the open clip through the same undo history.
    bool ensureSkeleton() { return loadSkeleton(); }
    const vats::Skeleton& skeleton() const { return mSkeleton; }
    const vats::Rig* rig() const { return mRig.get(); }
    vats::Pose shownPose() const;  // the pose the avatar shows now, in VATs' terms
    void replaceClip(vats::Clip clip, const std::string& name) { setClip(std::move(clip), name); refresh(); }

private:
    // Files
    void onOpenFile();
    void onFileChosen(const std::vector<std::string>& files);
    void onNew(bool from_pose);
    void onSave(bool as_anim);
    void onSaveChosen(const std::vector<std::string>& files, bool as_anim);
    // Playback
    void onPlayPause();
    void onStop();
    void onLoop();
    void onScrub(double frame);
    // Editing (6d)
    void onSetKey();
    void onDeleteKey();
    void onKeyPosed();
    void onUndo();
    void onRedo();
    void onLastFrame();
    // Upload (6c)
    void onUpload();
    void onUploadConfirmed(const LLSD& notification, const LLSD& response);

    bool loadSkeleton();
    void setClip(vats::Clip clip, const std::string& name);  // a newly opened or created clip
    bool rebind();   // points the preview at the clip's animated joints; false when it cannot play
    void edited();   // after any change to the clip: rebind, refresh the preview and the controls
    template <class F> void edit(const std::string& label, F&& change);
    bool keyFromAvatar(vats::Clip& clip, int node, int frame) const;  // key what the avatar shows
    void start();  // (re)starts the preview from the current frame
    void stop();   // removes the preview motion from the avatar
    void refresh();
    void setStatus(const std::string& text);

    vats::Skeleton mSkeleton;
    std::unique_ptr<vats::Rig> mRig;
    vats::Clip mClip;
    vats::History mHistory;
    std::string mName;  // file stem, the default for saving and uploading
    bool mHaveClip = false;
    unsigned mGeneration = 0;
    bool mPlayable = false;  // the clip exports and every joint it animates exists on the avatar
    LLUUID mMotionID;
    bool mMotionIsKeyframe = false;  // "Preview as uploaded": the viewer's own LLKeyframeMotion
    FSAnimTimelineCtrl* mTimeline = nullptr;
    FSAnimGraphCtrl* mGraph = nullptr;  // 6e
    LLScrollListCtrl* mJointList = nullptr;
    std::string mUploadBytes;  // the .anim the pending upload confirmation will send
};

#endif // FS_FLOATERANIMATOR_H
