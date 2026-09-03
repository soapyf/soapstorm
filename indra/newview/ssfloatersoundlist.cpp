/**
 * @file ssfloatersoundlist.cpp
 * @brief See ssfloatersoundlist.h.
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

#include "llviewerprecompiledheaders.h"

#include "ssfloatersoundlist.h"

#include "ssassetlist.h"

#include "llagent.h"
#include "llaudioengine.h"
#include "llbutton.h"
#include "llfloaterreg.h"
#include "lllocalcliprect.h"
#include "llinventory.h"
#include "lltooldraganddrop.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "lltextbox.h"
#include "lltimer.h"
#include "lltooltip.h"
#include "llui.h"
#include "llrender.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llwindow.h"

namespace
{
    const S32 ROW_H = 22;
    const S32 ROW_PAD = 2;
    const S32 ICON_W = 18;
    const S32 LEN_W = 52;
    const S32 REMOVE_W = 18;

    const S32 SCROLL_W = 6;

    const S32 DRAG_OUT_M = 46;

    const F32 UNKNOWN_LEN = 1.2f;
}

// Plays a sound as a UI-local source.
void ss_sound_play(const LLUUID& asset_id, const LLUUID& source_id)
{
    if (!gAudiop || asset_id.isNull()) return;

    ss_sound_stop(source_id);

    gAudiop->triggerSound(asset_id, gAgent.getID(), 1.f,
                          LLAudioEngine::AUDIO_TYPE_UI, LLVector3d::zero,
                          LLUUID::null, source_id);
}

// Stops a UI-local source.
void ss_sound_stop(const LLUUID& source_id)
{
    if (!gAudiop || source_id.isNull()) return;

    LLAudioSource* asp = gAudiop->findAudioSource(source_id);
    if (!asp) return;

    gAudiop->cleanupAudioSource(asp);
}

// A sound's length in seconds, when known.
F32 ss_sound_length(const LLUUID& id)
{
    if (id.isNull() || !gAudiop) return -1.f;

    LLAudioData* data = gAudiop->getAudioData(id);
    if (!data) return -1.f;

    if (!data->hasDecodedData())
    {
        gAudiop->preloadSound(id);
        return -1.f;
    }

    LLAudioBuffer* buffer = data->getBuffer();
    if (!buffer)
    {
        gAudiop->updateBufferForData(data, id);
        buffer = data->getBuffer();
    }
    if (!buffer) return -1.f;

    const U32 ms = buffer->getLengthMS();
    return (ms > 0) ? (F32)ms / 1000.f : -1.f;
}

static LLDefaultChildRegistry::Register<SSSoundListCtrl> r_ss_sound_list("ss_sound_list");

// Widget params.
SSSoundListCtrl::Params::Params()
:   mode("mode", "random"),
    max_sounds("max_sounds", 0)
{
}

// Compact control showing a sound list with an inline play button; clicking opens the editor.
SSSoundListCtrl::SSSoundListCtrl(const Params& p)
:   LLUICtrl(p),
    mMode(ss_asset_mode_from_key(p.mode)),
    mMaxSounds(llmax(0, p.max_sounds()))
{
}

// Rect of the inline play button.
LLRect SSSoundListCtrl::playRect() const
{
    const LLRect& r = getLocalRect();
    return LLRect(r.mRight - 20, r.mTop - 2, r.mRight - 2, r.mBottom + 2);
}

// Draws the summary strip, play state and hover highlight.
void SSSoundListCtrl::draw()
{
    advancePlayback();

    const LLRect& r = getLocalRect();

    gl_rect_2d(r, mHover ? LLColor4(0.19f, 0.20f, 0.25f, 1.f)
                         : LLColor4(0.13f, 0.13f, 0.16f, 1.f), true);
    gl_rect_2d(r, mHover ? LLColor4(0.55f, 0.58f, 0.68f, 1.f)
                         : LLColor4(0.35f, 0.35f, 0.40f, 1.f), false);

    LLUIImagePtr icon = LLUI::getUIImage("Inv_Sound");
    if (icon.notNull())
    {
        icon->draw(r.mLeft + 3, r.mBottom + (r.getHeight() - 16) / 2, 16, 16);
    }

    LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const S32 count = (S32)mList.size();

    const S32 text_left = r.mLeft + 23;
    const S32 text_room = llmax(0, playRect().mLeft - 4 - text_left);

    std::string label;
    if (count == 0)
    {
        label = (text_room >= 40) ? "Empty" : "-";
    }
    else
    {
        const std::string full = llformat("%d %s Sound%s", count,
                                          (mMode == SS_ASSET_SEQUENCE) ? "Sequenced" : "Random",
                                          count == 1 ? "" : "s");
        label = (font->getWidth(full) <= text_room) ? full : llformat("%d", count);
    }

    font->renderUTF8(label, 0, (F32)text_left, (F32)r.getCenterY(),
                     count ? LLColor4::white : LLColor4(0.6f, 0.6f, 0.6f, 1.f),
                     LLFontGL::LEFT, LLFontGL::VCENTER,
                     LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX,
                     text_room, NULL, true);

    const LLRect play = playRect();
    if (count > 0)
    {
        gl_rect_2d(play, mHoverPlay ? LLColor4(0.30f, 0.34f, 0.42f, 1.f)
                                    : LLColor4(0.20f, 0.22f, 0.28f, 1.f), true);

        LLUIImagePtr icon = LLUI::getUIImage(mPlaying ? "Pause_Off" : "Audio_Off");
        if (icon.notNull())
        {
            icon->draw(play.mLeft + 1, play.mBottom + (play.getHeight() - 14) / 2, 14, 14);
        }
    }

    LLUICtrl::draw();
}

// Clears hover.
void SSSoundListCtrl::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHover = false;
    mHoverPlay = false;
}

// Tracks hover for the highlight.
bool SSSoundListCtrl::handleHover(S32 x, S32 y, MASK mask)
{
    mHover = true;
    mHoverPlay = playRect().pointInRect(x, y);
    getWindow()->setCursor(UI_CURSOR_HAND);
    return true;
}

// Play button or open the editor.
bool SSSoundListCtrl::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (!mList.empty() && playRect().pointInRect(x, y))
    {
        if (mPlaying)
        {
            stopPlaying();
            return true;
        }

        startPlaying();
        return true;
    }

    openEditor();
    return true;
}

// Starts auditioning the list from the top.
void SSSoundListCtrl::startPlaying()
{
    if (mList.empty()) return;

    if (mVoice.isNull()) mVoice.generate();

    mPlaying = true;
    mPlayIndex = -1;
    mNextAt = 0.0;
}

// Stops the audition.
void SSSoundListCtrl::stopPlaying()
{
    ss_sound_stop(mVoice);

    mPlaying = false;
    mPlayIndex = -1;
}

// Steps the audition to the next sound when the current one ends.
void SSSoundListCtrl::advancePlayback()
{
    if (!mPlaying) return;

    if (mList.empty())
    {
        stopPlaying();
        return;
    }

    const F64 now = LLTimer::getElapsedSeconds();
    if (now < mNextAt) return;

    if (mMode == SS_ASSET_RANDOM)
    {
        mPlayIndex = (S32)((size_t)(ll_frand() * (F32)mList.size()) % mList.size());
    }
    else
    {
        mPlayIndex++;
        if (mPlayIndex >= (S32)mList.size())
        {
            stopPlaying();
            return;
        }
    }

    const LLUUID& id = mList[mPlayIndex];
    ss_sound_play(id, mVoice);

    const F32 len = ss_sound_length(id);
    mNextAt = now + (F64)((len > 0.f) ? len : UNKNOWN_LEN);
}

// Opens the shared editor floater bound to this control.
void SSSoundListCtrl::openEditor()
{
    SSFloaterSoundList* floater =
        LLFloaterReg::getTypedInstance<SSFloaterSoundList>("ss_sound_list");
    if (!floater) return;

    floater->editFor(this, mSlotLabel);
    floater->openFloater();
    mEditorHandle = floater->getHandle();
}

// Accepts sound drops straight onto the control.
bool SSSoundListCtrl::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                                       EDragAndDropType cargo_type, void* cargo_data,
                                       EAcceptance* accept, std::string& tooltip_msg)
{
    if (cargo_type != DAD_SOUND)
    {
        *accept = ACCEPT_NO;
        return false;
    }

    LLInventoryItem* item = (LLInventoryItem*)cargo_data;
    if (!item)
    {
        *accept = ACCEPT_NO;
        return false;
    }

    if (isFull())
    {
        *accept = ACCEPT_NO;
        tooltip_msg = llformat("This slot holds at most %d sounds", mMaxSounds);
        return true;
    }

    *accept = ACCEPT_YES_SINGLE;

    if (drop)
    {
        mList.push_back(item->getAssetUUID());
        if (gAudiop) gAudiop->preloadSound(item->getAssetUUID());

        onCommit();
    }

    return true;
}

static LLDefaultChildRegistry::Register<SSSoundListRows> r_ss_sound_list_rows("ss_sound_list_rows");

// Widget params.
SSSoundListRows::Params::Params()
{
}

// The editor's scrolling row view.
SSSoundListRows::SSSoundListRows(const Params& p)
:   LLUICtrl(p)
{
}

// Total height of all rows.
S32 SSSoundListRows::contentHeight() const
{
    return (S32)mList.size() * (ROW_H + ROW_PAD);
}

// Scroll range.
S32 SSSoundListRows::maxScroll() const
{
    return llmax(0, contentHeight() - getLocalRect().getHeight());
}

// Keeps scroll in range.
void SSSoundListRows::clampScroll()
{
    mScroll = llclamp(mScroll, 0, maxScroll());
}

// Scrolls a row into view (the audition follows playback).
void SSSoundListRows::scrollTo(S32 index)
{
    if (index < 0 || index >= (S32)mList.size()) return;

    const S32 top = index * (ROW_H + ROW_PAD);
    const S32 bottom = top + ROW_H;
    const S32 view_h = getLocalRect().getHeight();

    if (top < mScroll) mScroll = top;
    else if (bottom > mScroll + view_h) mScroll = bottom - view_h;

    clampScroll();
}

// Rect of a row.
LLRect SSSoundListRows::rowRect(S32 index) const
{
    const LLRect& r = getLocalRect();
    const S32 top = r.mTop + mScroll - index * (ROW_H + ROW_PAD);
    return LLRect(r.mLeft, top, r.mRight - SCROLL_W, top - ROW_H);
}

// Rect of a row's remove button.
LLRect SSSoundListRows::removeRect(S32 index) const
{
    const LLRect row = rowRect(index);
    return LLRect(row.mRight - REMOVE_W - 4, row.mTop - 2,
                  row.mRight - 4, row.mBottom + 2);
}

// Row under a y.
S32 SSSoundListRows::rowAt(S32 y) const
{
    const LLRect& r = getLocalRect();
    const S32 index = (r.mTop + mScroll - y) / (ROW_H + ROW_PAD);
    return (index >= 0 && index < (S32)mList.size()) ? index : -1;
}

// Insertion gap under a y, for drag reordering.
S32 SSSoundListRows::gapAt(S32 y) const
{
    const LLRect& r = getLocalRect();
    const S32 gap = (S32)((F32)(r.mTop + mScroll - y) / (F32)(ROW_H + ROW_PAD) + 0.5f);
    return llclamp(gap, 0, (S32)mList.size());
}

// List changed: clamp scroll and notify.
void SSSoundListRows::changed()
{
    if (mOnChanged) mOnChanged();
}

// Draws rows with names, lengths, playing marker, remove buttons and the drag insertion marker.
void SSSoundListRows::draw()
{
    const LLRect& r = getLocalRect();
    gl_rect_2d(r, LLColor4(0.10f, 0.10f, 0.12f, 1.f), true);

    LLFontGL* font = LLFontGL::getFontSansSerifSmall();

    LLLocalClipRect clip(r);

    for (S32 i = 0; i < (S32)mList.size(); ++i)
    {
        const LLRect row = rowRect(i);
        if (row.mBottom > r.mTop) continue;
        if (row.mTop < r.mBottom) break;

        const bool dragging_this = (mDragFrom == i);

        const F32 alpha = (dragging_this && mDragOut) ? 0.3f
                        : (dragging_this ? 0.65f : 1.f);

        const bool hovered = (i == mHoverRow) && (mDragFrom < 0);
        const LLColor4 row_col = hovered ? LLColor4(0.23f, 0.24f, 0.30f, alpha)
                                         : LLColor4(0.16f, 0.16f, 0.20f, alpha);
        gl_rect_2d(row, row_col, true);

        if (i == mPlaying)
        {
            LLUIImagePtr icon = LLUI::getUIImage("Audio_Off");
            if (icon.notNull())
            {
                icon->draw(row.mLeft + 2, row.mBottom + (ROW_H - 14) / 2, 14, 14);
            }
        }

        const std::string name = ss_asset_name(mList[i]);
        const bool named = !name.empty();
        const std::string row_text = named ? name : mList[i].asString();

        const LLColor4 text_col = named
            ? LLColor4(0.88f, 0.88f, 0.92f, alpha)
            : LLColor4(0.62f, 0.62f, 0.68f, alpha);

        const S32 text_left = row.mLeft + ICON_W + 6;
        const S32 text_right = row.mRight - REMOVE_W - LEN_W - 12;
        font->renderUTF8(row_text, 0, (F32)text_left, (F32)row.getCenterY(),
                         text_col, LLFontGL::LEFT, LLFontGL::VCENTER,
                         LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX,
                         llmax(0, text_right - text_left), NULL, true);

        const F32 secs = ss_sound_length(mList[i]);
        const std::string len_text = (secs >= 0.f) ? llformat("%.2fs", secs)
                                                   : std::string("--");
        font->renderUTF8(len_text, 0, row.mRight - REMOVE_W - 10, row.getCenterY(),
                         LLColor4(0.65f, 0.65f, 0.7f, alpha),
                         LLFontGL::RIGHT, LLFontGL::VCENTER);

        const LLRect x_rect = removeRect(i);
        const bool x_hot = hovered && mHoverRemove;
        if (x_hot)
        {
            gl_rect_2d(x_rect, LLColor4(0.45f, 0.20f, 0.20f, alpha), true);
        }
        font->renderUTF8("x", 0, x_rect.getCenterX(), x_rect.getCenterY(),
                         x_hot ? LLColor4(1.f, 0.85f, 0.85f, alpha)
                               : LLColor4(0.62f, 0.55f, 0.55f, alpha),
                         LLFontGL::HCENTER, LLFontGL::VCENTER);
    }

    if (maxScroll() > 0)
    {
        const S32 view_h = r.getHeight();
        const S32 track_x = r.mRight - SCROLL_W;

        gl_rect_2d(LLRect(track_x, r.mTop, r.mRight, r.mBottom),
                   LLColor4(0.06f, 0.06f, 0.08f, 1.f), true);

        const F32 span = (F32)view_h / (F32)contentHeight();
        const S32 thumb_h = llmax(20, (S32)(span * (F32)view_h));
        const F32 pos = (F32)mScroll / (F32)maxScroll();
        const S32 thumb_top = r.mTop - (S32)(pos * (F32)(view_h - thumb_h));

        gl_rect_2d(LLRect(track_x + 1, thumb_top, r.mRight - 1, thumb_top - thumb_h),
                   LLColor4(0.34f, 0.36f, 0.42f, 1.f), true);
    }

    const S32 marker = (mDropGap >= 0) ? mDropGap : (mDragFrom >= 0 ? mDragTo : -1);
    if (marker >= 0 && !mDragOut)
    {
        const S32 y = r.mTop + mScroll - marker * (ROW_H + ROW_PAD) + ROW_PAD / 2;
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.color4f(0.4f, 0.75f, 1.f, 0.95f);
        gGL.begin(LLRender::LINES);
        gGL.vertex2i(r.mLeft + 2, y);
        gGL.vertex2i(r.mRight - SCROLL_W - 2, y);
        gGL.end();
        gGL.flush();
    }

    LLUICtrl::draw();
}

// Remove click or drag start.
bool SSSoundListRows::handleMouseDown(S32 x, S32 y, MASK mask)
{
    const S32 row = rowAt(y);
    if (row < 0) return LLUICtrl::handleMouseDown(x, y, mask);

    if (removeRect(row).pointInRect(x, y))
    {
        mList.erase(mList.begin() + row);
        if (mPlaying >= (S32)mList.size()) mPlaying = -1;
        changed();
        return true;
    }

    mDragFrom = row;
    mDragTo = row;
    mDragOut = false;
    gFocusMgr.setMouseCapture(this);
    return true;
}

// Scrolls the rows.
bool SSSoundListRows::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    if (maxScroll() <= 0) return false;

    mScroll += clicks * (ROW_H + ROW_PAD);
    clampScroll();
    return true;
}

// Full name tooltip per row.
bool SSSoundListRows::handleToolTip(S32 x, S32 y, MASK mask)
{
    const S32 row = rowAt(y);
    if (row >= 0 && row < (S32)mList.size())
    {
        LLToolTipMgr::instance().show(mList[row].asString());
        return true;
    }
    return LLUICtrl::handleToolTip(x, y, mask);
}

// Clears hover.
void SSSoundListRows::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHoverRow = -1;
    mHoverRemove = false;
}

// Hover and drag tracking.
bool SSSoundListRows::handleHover(S32 x, S32 y, MASK mask)
{
    if (mDragFrom < 0 || !hasMouseCapture())
    {
        mHoverRow = rowAt(y);
        mHoverRemove = (mHoverRow >= 0) && removeRect(mHoverRow).pointInRect(x, y);
        getWindow()->setCursor(mHoverRow >= 0 ? UI_CURSOR_HAND : UI_CURSOR_ARROW);
        return LLUICtrl::handleHover(x, y, mask);
    }

    mHoverRow = -1;
    mHoverRemove = false;

    const LLRect& r = getLocalRect();

    mDragOut = (x < r.mLeft - DRAG_OUT_M) || (x > r.mRight + DRAG_OUT_M)
            || (y > r.mTop + DRAG_OUT_M) || (y < r.mBottom - DRAG_OUT_M);

    mDragTo = gapAt(y);
    getWindow()->setCursor(mDragOut ? UI_CURSOR_NOLOCKED : UI_CURSOR_ARROW);
    return true;
}

// Finishes a drag reorder (or plays the clicked row).
bool SSSoundListRows::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (mDragFrom < 0 || !hasMouseCapture()) return LLUICtrl::handleMouseUp(x, y, mask);

    gFocusMgr.setMouseCapture(NULL);

    const S32 from = mDragFrom;
    const S32 to = mDragTo;
    const bool out = mDragOut;

    mDragFrom = -1;
    mDragTo = -1;
    mDragOut = false;

    if (out)
    {
        mList.erase(mList.begin() + from);
    }
    else if (to != from && to != from + 1)
    {
        const LLUUID id = mList[from];
        mList.erase(mList.begin() + from);
        mList.insert(mList.begin() + (to > from ? to - 1 : to), id);
    }
    else
    {
        return true;
    }

    if (mPlaying >= (S32)mList.size()) mPlaying = -1;
    clampScroll();
    changed();
    return true;
}

// Accepts inventory sound drops at the hovered gap.
bool SSSoundListRows::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                                       EDragAndDropType cargo_type, void* cargo_data,
                                       EAcceptance* accept, std::string& tooltip_msg)
{
    if (cargo_type != DAD_SOUND)
    {
        mDropGap = -1;
        *accept = ACCEPT_NO;
        return false;
    }

    LLInventoryItem* item = (LLInventoryItem*)cargo_data;
    if (!item)
    {
        mDropGap = -1;
        *accept = ACCEPT_NO;
        return false;
    }

    if (isFull())
    {
        mDropGap = -1;
        *accept = ACCEPT_NO;
        tooltip_msg = llformat("This slot holds at most %d sounds", mMaxSounds);
        return true;
    }

    *accept = ACCEPT_YES_SINGLE;
    mDropGap = gapAt(y);

    if (drop)
    {
        LLToolDragAndDrop& dnd = LLToolDragAndDrop::instance();
        const S32 index = dnd.getCargoIndex();
        const S32 count = (S32)dnd.getCargoCount();

        if (index <= 0)
        {
            mBatchStart = llclamp(mDropGap, 0, (S32)mList.size());
            mBatchCount = 0;
        }

        const LLUUID id = item->getAssetUUID();
        const S32 at = llclamp(mBatchStart + mBatchCount, 0, (S32)mList.size());
        mList.insert(mList.begin() + at, id);
        mBatchCount++;

        if (gAudiop) gAudiop->preloadSound(id);

        if (index + 1 >= count && mBatchCount > 1)
        {
            std::sort(mList.begin() + mBatchStart,
                      mList.begin() + mBatchStart + mBatchCount,
                      [](const LLUUID& x, const LLUUID& y)
                      {
                          const std::string nx = ss_asset_name(x);
                          const std::string ny = ss_asset_name(y);
                          return ss_natural_less(nx.empty() ? x.asString() : nx,
                                                 ny.empty() ? y.asString() : ny);
                      });
        }

        mDropGap = -1;
        changed();
    }

    return true;
}

// Editor floater shell.
SSFloaterSoundList::SSFloaterSoundList(const LLSD& key)
:   LLFloater(key)
{
}

SSFloaterSoundList::~SSFloaterSoundList()
{
}

// Wires the rows view, mode combo, CSV line and buttons.
bool SSFloaterSoundList::postBuild()
{
    mList = getChild<SSSoundListRows>("sound_list");

    getChild<LLUICtrl>("play_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onClickPlay(); });
    getChild<LLUICtrl>("ok_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onClickOK(); });
    getChild<LLUICtrl>("cancel_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onClickCancel(); });

    getChild<LLUICtrl>("csv_editor")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitCsv(); });

    mList->setOnChanged([this]() { refresh(); });
    return true;
}

// Binds the editor to a control, snapshotting the list for cancel.
void SSFloaterSoundList::editFor(SSSoundListCtrl* owner, const std::string& label)
{
    if (!owner) return;

    mOwnerHandle = owner->getHandle();
    mOriginal = owner->getList();
    mMode = owner->getMode();
    mCancelled = false;
    mList->setList(mOriginal);
    mList->setMaxSounds(owner->getMaxSounds());

    getChild<LLTextBox>("mode_text")->setText(std::string(
        (mMode == SS_ASSET_SEQUENCE)
            ? "Plays through in order"
            : "Plays one at random each time"));

    setTitle(label.empty() ? std::string("Sounds") : ("Sounds - " + label));

    stopPlayback();
    refresh();
}

// Close behaves as cancel and stops playback.
void SSFloaterSoundList::onClose(bool app_quitting)
{
    stopPlayback();

    if (!mCancelled)
    {
        commitToOwner();
    }
    mCancelled = false;
}

// Pushes the edited list back to the owning control.
void SSFloaterSoundList::commitToOwner()
{
    SSSoundListCtrl* owner = dynamic_cast<SSSoundListCtrl*>(mOwnerHandle.get());
    if (owner && mList)
    {
        owner->setList(mList->getList());
        owner->onCommit();
    }
}

// Restores the snapshot on cancel.
void SSFloaterSoundList::restoreOwner()
{
    SSSoundListCtrl* owner = dynamic_cast<SSSoundListCtrl*>(mOwnerHandle.get());
    if (owner)
    {
        owner->setList(mOriginal);
        owner->onCommit();
    }
}

// Parses a pasted CSV into the list.
void SSFloaterSoundList::onCommitCsv()
{
    const std::string csv = getChild<LLUICtrl>("csv_editor")->getValue().asString();
    if (mList)
    {
        mList->setList(ss_asset_list_parse(csv));
    }

    stopPlayback();
    refresh();
}

// Rewrites rows and CSV from the list.
void SSFloaterSoundList::refresh()
{
    const S32 count = mList ? (S32)mList->getList().size() : 0;
    LLUICtrl* csv = getChild<LLUICtrl>("csv_editor");
    if (mList && !csv->hasFocus())
    {
        csv->setValue(ss_asset_list_str(mList->getList()));
    }

    const S32 cap = mList ? (mList->isFull() ? count : 0) : 0;
    getChild<LLTextBox>("count_text")->setText(
        cap > 0 ? llformat("%d sounds (full)", count)
                : llformat("%d sound%s", count, count == 1 ? "" : "s"));

    getChild<LLUICtrl>("play_button")->setEnabled(count > 0 || mPlaying);
    getChild<LLButton>("play_button")->setLabel(std::string(mPlaying ? "Stop" : "Play"));
}

// Starts or stops the audition.
void SSFloaterSoundList::onClickPlay()
{
    if (mPlaying)
    {
        stopPlayback();
        return;
    }

    if (!mList || mList->getList().empty()) return;

    if (mVoice.isNull()) mVoice.generate();

    mPlaying = true;
    mPlayIndex = -1;
    mNextAt = 0.0;
    refresh();
}

// Stops the audition.
void SSFloaterSoundList::stopPlayback()
{
    ss_sound_stop(mVoice);

    mPlaying = false;
    mPlayIndex = -1;
    if (mList) mList->setPlaying(-1);
    refresh();
}

// Steps the audition through the list as sounds finish.
void SSFloaterSoundList::advancePlayback()
{
    if (!mPlaying || !mList) return;

    const SSSoundList& seq = mList->getList();
    if (seq.empty())
    {
        stopPlayback();
        return;
    }

    const F64 now = LLTimer::getElapsedSeconds();
    if (now < mNextAt) return;

    if (mMode == SS_ASSET_RANDOM)
    {
        mPlayIndex = (S32)((size_t)(ll_frand() * (F32)seq.size()) % seq.size());
    }
    else
    {
        mPlayIndex++;
        if (mPlayIndex >= (S32)seq.size())
        {
            stopPlayback();
            return;
        }
    }

    const LLUUID& id = seq[mPlayIndex];
    ss_sound_play(id, mVoice);

    const F32 len = ss_sound_length(id);
    mNextAt = now + (F64)((len > 0.f) ? len : UNKNOWN_LEN);

    mList->setPlaying(mPlayIndex);
}

// Drives the audition advance.
void SSFloaterSoundList::draw()
{
    advancePlayback();
    LLFloater::draw();
}

// Commit and close.
void SSFloaterSoundList::onClickOK()
{
    commitToOwner();
    closeFloater();
}

// Restore and close.
void SSFloaterSoundList::onClickCancel()
{
    restoreOwner();

    mCancelled = true;
    closeFloater();
}
