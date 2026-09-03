/**
 * @file ssfloatersoundlist.h
 * @brief Atmo Magic: sound list control and editor.
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

#ifndef SS_FLOATERSOUNDLIST_H
#define SS_FLOATERSOUNDLIST_H

#include "ssassetlist.h"

#include "llfloater.h"
#include "lluictrl.h"

#include <functional>
#include <string>
#include <vector>

typedef SSAssetList SSSoundList;
typedef SSAssetListMode SSSoundListMode;

void ss_sound_play(const LLUUID& asset_id, const LLUUID& source_id);
void ss_sound_stop(const LLUUID& source_id);

F32 ss_sound_length(const LLUUID& id);

std::string ss_asset_name(const LLUUID& id);

class SSSoundListCtrl : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Optional<std::string> mode;
        Optional<S32> max_sounds;

        Params();
    };

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;

    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                           EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;

    void setList(const SSSoundList& seq) { mList = seq; stopPlaying(); }
    const SSSoundList& getList() const { return mList; }

    void setMode(SSSoundListMode mode) { mMode = mode; }
    SSSoundListMode getMode() const { return mMode; }

    void setMaxSounds(S32 n) { mMaxSounds = llmax(0, n); }
    S32 getMaxSounds() const { return mMaxSounds; }
    bool isFull() const { return mMaxSounds > 0 && (S32)mList.size() >= mMaxSounds; }

    void setSlotLabel(const std::string& label) { mSlotLabel = label; }

protected:
    friend class LLUICtrlFactory;
    SSSoundListCtrl(const Params& p);

private:
    void openEditor();
    LLRect playRect() const;

    void startPlaying();
    void stopPlaying();
    void advancePlayback();

    bool mPlaying = false;
    S32 mPlayIndex = -1;
    F64 mNextAt = 0.0;

    LLUUID mVoice;

    SSSoundList mList;
    SSSoundListMode mMode = SS_ASSET_RANDOM;
    S32 mMaxSounds = 0;
    std::string mSlotLabel;

    void onMouseLeave(S32 x, S32 y, MASK mask) override;

    LLHandle<LLFloater> mEditorHandle;
    bool mHover = false;
    bool mHoverPlay = false;
};

class SSSoundListRows : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params();
    };

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                           EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;

    void setList(const SSSoundList& seq) { mList = seq; mPlaying = -1; }
    const SSSoundList& getList() const { return mList; }

    void setOnChanged(std::function<void()> cb) { mOnChanged = cb; }

    void setMaxSounds(S32 n) { mMaxSounds = llmax(0, n); }
    bool isFull() const { return mMaxSounds > 0 && (S32)mList.size() >= mMaxSounds; }

    void setPlaying(S32 index) { mPlaying = index; scrollTo(index); }
    S32 getPlaying() const { return mPlaying; }

protected:
    friend class LLUICtrlFactory;
    SSSoundListRows(const Params& p);

private:
    S32 contentHeight() const;
    S32 maxScroll() const;
    void clampScroll();
    void scrollTo(S32 index);

    void onMouseLeave(S32 x, S32 y, MASK mask) override;

    S32 rowAt(S32 y) const;
    S32 gapAt(S32 y) const;
    LLRect rowRect(S32 index) const;
    LLRect removeRect(S32 index) const;
    void changed();

    SSSoundList mList;
    S32 mPlaying = -1;

    S32 mHoverRow = -1;
    bool mHoverRemove = false;

    S32 mScroll = 0;

    S32 mMaxSounds = 0;

    S32 mDragFrom = -1;
    S32 mDragTo = -1;
    bool mDragOut = false;

    S32 mDropGap = -1;

    S32 mBatchStart = 0;
    S32 mBatchCount = 0;

    std::function<void()> mOnChanged;
};

class SSFloaterSoundList : public LLFloater
{
public:
    SSFloaterSoundList(const LLSD& key);
    ~SSFloaterSoundList();

    bool postBuild() override;
    void onClose(bool app_quitting) override;
    void draw() override;

    void editFor(SSSoundListCtrl* owner, const std::string& label);

    SSSoundListMode mode() const { return mMode; }

private:
    void commitToOwner();
    void restoreOwner();

    void onCommitCsv();
    void onClickPlay();
    void stopPlayback();
    void onClickOK();
    void onClickCancel();
    void advancePlayback();
    void refresh();

    SSSoundListRows* mList = nullptr;
    LLHandle<LLView> mOwnerHandle;

    SSSoundList mOriginal;

    SSSoundListMode mMode = SS_ASSET_RANDOM;

    bool mCancelled = false;

    bool mPlaying = false;
    S32 mPlayIndex = -1;
    F64 mNextAt = 0.0;

    LLUUID mVoice;
};

#endif
