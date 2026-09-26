/**
 * @file ssfloatersoundanalysis.cpp
 * @brief See ssfloatersoundanalysis.h.
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

#include "ssfloatersoundanalysis.h"

#include "sssoundmeta.h"
#include "ssatmomagic.h"
#include "ssassetlist.h"
#include "ssfloatersoundlist.h"

#include "llagent.h"
#include "llaudioengine.h"
#include "llfontgl.h"
#include "llrand.h"
#include "llrender2dutils.h"
#include "llscrollcontainer.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llviewercamera.h"
#include "llwindow.h"

#include <set>

namespace
{
    const S32 ROW_H = 66;
    const S32 GROUP_H = 20;
    const S32 WAVE_H = 34;
    const S32 PAD = 8;
    const S32 PLAY_BTN = 16;

    // The segmentability gate the live step loop demands before a recording is played as per-impact cuts.
    bool step_cut_capable(const SSSoundMeta::Meta& meta)
    {
        return meta.mOnsets.size() >= 4
            && meta.mGapFloor < 0.12f
            && meta.mCadenceCV < 0.4f
            && meta.mImpactRate > 0.8f && meta.mImpactRate < 4.5f;
    }

    // The bare playback context a purpose names, for rows with no analysis to refine it yet.
    std::string rowContextWord(U32 purpose)
    {
        if (purpose & SSSoundMeta::PURPOSE_STEPS)   return "steps";
        if (purpose & SSSoundMeta::PURPOSE_TIMING)  return "thunder";
        if (purpose & SSSoundMeta::PURPOSE_DENSITY) return "ambience";
        return std::string();
    }

    // The full playback mode once the analysis is in: the same derivation the preview and
    // the live soundscape use, so the label and the button behaviour cannot disagree.
    std::string rowModeLabel(U32 purpose, const SSSoundMeta::Meta* meta)
    {
        if (purpose & SSSoundMeta::PURPOSE_STEPS)
        {
            return (meta && step_cut_capable(*meta)) ? "steps/cut" : "steps/loop";
        }
        return rowContextWord(purpose);
    }
}

class SSSoundAnalysisView : public LLView
{
public:
    SSSoundAnalysisView(const LLView::Params& p) : LLView(p) {}

    // Content height for the scroll container, via the same layout draw uses - the entry map's
    // UUID order scatters same-source entries, so counting groups off the raw map overcounts
    // and desyncs the scrollbar from the drawn rows.
    S32 neededHeight() const
    {
        std::vector<RowInfo> rows;
        buildRows(rows);
        S32 groups = 0;
        for (const RowInfo& row : rows) { if (row.mGroupStart) ++groups; }
        return groups * GROUP_H + (S32)rows.size() * ROW_H + PAD * 2;
    }

    // Renders every READY sound as a stat line, its envelope with markers, a preview button and a live playhead.
    void draw() override;

    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;

    // Hard stop - used when the floater goes away, leaves no voices behind.
    void stopPreview();

private:
    // One laid-out row; shared by the renderer and the hit tests so they can never disagree.
    struct RowInfo
    {
        LLUUID mID;                             // null for a configured-but-empty slot
        const SSSoundMeta::Meta* mMeta = nullptr;
        SSSoundMeta::EState mState = SSSoundMeta::PENDING;
        U32 mPurpose = 0;
        std::string mSource;
        std::string mFailWhy;
        bool mGroupStart = false;
        S32 mY = 0;
    };

    void buildRows(std::vector<RowInfo>& rows) const;
    LLRect playRect(const RowInfo& row) const;
    S32 playRowAt(S32 x, S32 y);

    enum EMode { PREVIEW_NONE, PREVIEW_LOOP, PREVIEW_ONESHOT, PREVIEW_STEPS };

    struct Preview
    {
        LLUUID mSound;
        EMode mMode = PREVIEW_NONE;
        LLUUID mSourceID;
        F32 mGain = 1.f;

        // Ambience slots listing several sounds play as a sequence, exactly as the
        // soundscape's applyLoop cycles them: one non-looped pass per asset, then the
        // next in definition order, wrapping. Single-sound slots loop that one asset.
        std::vector<LLUUID> mQueue;
        S32 mQueueIdx = 0;

        F64 mStartedAt = 0.0;   // loop/oneshot wall-clock anchor
        U32 mStartMS = 0;       // loop resume offset
        F64 mEndsAt = 0.0;      // oneshot natural end / sequence advance deadline

        F64 mCutStartedAt = 0.0;    // the cut currently sounding
        F64 mCutStopAt = 0.0;
        U32 mCutStartMS = 0;
        U32 mCutEndMS = 0;
        F64 mNextImpactAt = 0.0;    // the cadence clock's next footfall
    };

    void togglePreview(const RowInfo& row);
    void fadeOutPreview();
    void startCut(F64 when, const SSSoundMeta::Meta& meta);
    void startSequenceAsset(F64 now);
    void updatePreview(F64 now);
    LLUUID startSource(const LLUUID& sound, F32 gain, bool loop, U32 offset_ms);
    void fadeKill(const LLUUID& source_id, F64 now);
    void cleanupDying(F64 now);

    Preview mPreview;
    std::vector<std::pair<LLUUID, F64>> mDying;
    S32 mHoverRow = -1;
};

// Walks the rows exactly as draw renders them: slots in definition order, each slot's
// sounds in its sequence (CSV) order, empty slots in place, plus any stale entries no
// longer in a slot appended after. Every entry state appears - pending, analysing,
// failed - so nothing configured is silently absent.
void SSSoundAnalysisView::buildRows(std::vector<RowInfo>& rows) const
{
    rows.clear();

    const auto& entries = SSSoundMeta::getInstance()->entriesForDebug();
    std::set<LLUUID> placed;

    for (const SSSoundMeta::SlotInfo& slot : SSSoundMeta::getInstance()->slotsForDebug())
    {
        for (const LLUUID& id : slot.mSounds)
        {
            if (!placed.insert(id).second) continue;
            const auto it = entries.find(id);
            if (it == entries.end()) continue;

            RowInfo row;
            row.mID = it->first;
            row.mState = it->second.mState;
            row.mMeta = (it->second.mState == SSSoundMeta::READY) ? &it->second.mMeta : nullptr;
            row.mPurpose = it->second.mPurpose;
            row.mSource = slot.mSource;
            row.mFailWhy = it->second.mFailWhy;
            rows.push_back(row);
        }

        if (!slot.mSounds.empty()) continue;

        RowInfo row;
        row.mState = SSSoundMeta::EMPTY;
        row.mPurpose = slot.mPurpose;
        row.mSource = slot.mSource;
        rows.push_back(row);
    }

    std::vector<RowInfo> stale;
    for (const auto& pair : entries)
    {
        if (placed.count(pair.first)) continue;
        RowInfo row;
        row.mID = pair.first;
        row.mState = pair.second.mState;
        row.mMeta = (pair.second.mState == SSSoundMeta::READY) ? &pair.second.mMeta : nullptr;
        row.mPurpose = pair.second.mPurpose;
        row.mSource = pair.second.mSource;
        row.mFailWhy = pair.second.mFailWhy;
        stale.push_back(row);
    }
    std::sort(stale.begin(), stale.end(),
              [](const RowInfo& a, const RowInfo& b) { return a.mSource < b.mSource; });
    rows.insert(rows.end(), stale.begin(), stale.end());

    S32 y = getRect().getHeight() - PAD;
    std::string last_group;
    for (RowInfo& row : rows)
    {
        row.mGroupStart = (row.mSource != last_group);
        if (row.mGroupStart) { last_group = row.mSource; y -= GROUP_H; }
        y -= ROW_H;
        row.mY = y;
    }
}

// Rect of a row's preview button, sitting on the stat line's left edge.
LLRect SSSoundAnalysisView::playRect(const RowInfo& row) const
{
    return LLRect(PAD, row.mY + ROW_H - 4, PAD + PLAY_BTN, row.mY + ROW_H - 4 - PLAY_BTN);
}

// The row whose preview button holds the point, -1 when none. Only analysed sounds have a button.
S32 SSSoundAnalysisView::playRowAt(S32 x, S32 y)
{
    std::vector<RowInfo> rows;
    buildRows(rows);
    for (S32 i = 0; i < (S32)rows.size(); ++i)
    {
        if (!rows[i].mMeta) continue;
        if (playRect(rows[i]).pointInRect(x, y)) return i;
    }
    return -1;
}

void SSSoundAnalysisView::draw()
{
    const F64 now = SSAtmoMagic::getInstance()->sharedTime();
    cleanupDying(now);
    updatePreview(now);

    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const S32 width = getRect().getWidth();

    std::vector<RowInfo> rows;
    buildRows(rows);

    for (S32 i = 0; i < (S32)rows.size(); ++i)
    {
        const RowInfo& row = rows[i];
        const SSSoundMeta::Meta* meta = row.mMeta;
        const bool ready = (meta != nullptr);

        if (row.mGroupStart)
        {
            bold->renderUTF8(row.mSource.empty() ? std::string("(unattributed)") : row.mSource,
                             0, PAD, row.mY + ROW_H + 5, LLColor4(1.f, 0.85f, 0.4f, 1.f),
                             LLFontGL::LEFT, LLFontGL::BASELINE);
        }

        const std::string name = row.mID.notNull() ? ss_asset_name(row.mID) : std::string();
        const std::string title = row.mID.notNull()
            ? (name.empty() ? row.mID.asString().substr(0, 12) : name)
            : std::string("(unset)");

        // The playback context this sound was interpreted as - same derivation the preview and
        // the live soundscape use: step recordings split by the segmentability gate, thunder
        // by its timing purpose, ambience by density. Unanalysed rows show the bare context.
        const std::string context = ready ? rowModeLabel(row.mPurpose, meta)
                                          : rowContextWord(row.mPurpose);

        S32 text_x = PAD + PLAY_BTN + 6;
        if (!context.empty())
        {
            font->renderUTF8(context, 0, text_x, row.mY + ROW_H - 12,
                             ready ? LLColor4(0.5f, 0.8f, 1.f, 0.9f) : LLColor4(0.45f, 0.6f, 0.75f, 0.7f),
                             LLFontGL::LEFT, LLFontGL::BASELINE);
            text_x += font->getWidth(context) + 10;
        }

        if (ready)
        {
            font->renderUTF8(llformat("%s   len %.1fs  onset %.2fs  tail %.1fs  level %.2f  imp/s %.1f  dens %.2f  gap %.2f  cv %.2f  fix %d  crack %.2f",
                                      title.c_str(), meta->mLengthMS / 1000.f, meta->mOnsetMS / 1000.f,
                                      meta->mTailMS / 1000.f, meta->mPeakLevel, meta->mImpactRate, meta->mDensity, meta->mGapFloor, meta->mCadenceCV, (S32)meta->mRepaired, meta->mCrackiness),
                             0, text_x, row.mY + ROW_H - 12, LLColor4(0.9f, 0.9f, 0.9f, 1.f),
                             LLFontGL::LEFT, LLFontGL::BASELINE);
        }
        else
        {
            std::string status;
            LLColor4 col;
            switch (row.mState)
            {
                case SSSoundMeta::EMPTY:
                    status = "unset - nothing configured in this slot";
                    col = LLColor4(0.42f, 0.42f, 0.46f, 0.85f);
                    break;
                case SSSoundMeta::FAILED:
                    status = row.mFailWhy.empty() ? "analysis failed" : llformat("failed - %s", row.mFailWhy.c_str());
                    col = LLColor4(1.f, 0.45f, 0.4f, 0.9f);
                    break;
                case SSSoundMeta::ANALYZING:
                    status = "analysing...";
                    col = LLColor4(0.55f, 0.55f, 0.6f, 0.85f);
                    break;
                default:
                    status = "pending - waiting for decode";
                    col = LLColor4(0.55f, 0.55f, 0.6f, 0.85f);
                    break;
            }
            font->renderUTF8(llformat("%s   %s", title.c_str(), status.c_str()),
                             0, text_x, row.mY + ROW_H - 12, col,
                             LLFontGL::LEFT, LLFontGL::BASELINE);
        }

        const S32 wave_top = row.mY + WAVE_H + 6;
        const S32 wave_bottom = row.mY + 6;
        const S32 wave_w = width - PAD * 2;
        gl_rect_2d(PAD, wave_top, PAD + wave_w, wave_bottom, LLColor4(0.07f, 0.07f, 0.09f, 1.f));

        if (ready && !meta->mEnvelope.empty() && meta->mLengthMS > 0)
        {
            const S32 n = (S32)meta->mEnvelope.size();
            for (S32 e = 0; e < n; ++e)
            {
                const S32 x0 = PAD + e * wave_w / n;
                const S32 x1 = PAD + (e + 1) * wave_w / n;
                const S32 h = (S32)(meta->mEnvelope[(size_t)e] * (WAVE_H - 2));
                gl_rect_2d(x0, wave_bottom + 1 + h, x1, wave_bottom + 1, LLColor4(0.35f, 0.55f, 0.75f, 1.f));
            }

            auto ms_to_x = [&](U32 ms) { return PAD + (S32)((U64)ms * wave_w / meta->mLengthMS); };

            for (U32 ms : meta->mOnsets)
            {
                const S32 x = ms_to_x(ms);
                gl_rect_2d(x, wave_bottom + 8, x + 1, wave_bottom + 1, LLColor4(1.f, 1.f, 1.f, 0.7f));
            }

            const S32 px = ms_to_x(meta->mPeakMS);
            gl_rect_2d(px - 1, wave_top, px + 1, wave_bottom, LLColor4(1.f, 0.7f, 0.15f, 0.55f));
            const S32 tx = ms_to_x(meta->mTailMS);
            gl_rect_2d(tx, wave_top, tx + 1, wave_bottom, LLColor4(1.f, 0.25f, 0.2f, 0.9f));
            const S32 ox = ms_to_x(meta->mOnsetMS);
            gl_rect_2d(ox, wave_top, ox + 1, wave_bottom, LLColor4(0.2f, 1.f, 0.35f, 0.95f));

            if (row.mID == mPreview.mSound && mPreview.mMode != PREVIEW_NONE)
            {
                F32 pos_ms = -1.f;
                if (mPreview.mMode == PREVIEW_ONESHOT)
                {
                    pos_ms = (F32)llclamp((now - mPreview.mStartedAt) * 1000.0, 0.0, (F64)meta->mLengthMS);
                }
                else if (mPreview.mMode == PREVIEW_LOOP)
                {
                    pos_ms = (F32)fmod((F64)mPreview.mStartMS + (now - mPreview.mStartedAt) * 1000.0, (F64)meta->mLengthMS);
                }
                else if (mPreview.mSourceID.notNull() && now >= mPreview.mCutStartedAt && now <= mPreview.mCutStopAt)
                {
                    // Steps: the line rides the sounding cut, jumping onset to onset the way the gait fires them.
                    pos_ms = (F32)mPreview.mCutStartMS
                           + (F32)llclamp((now - mPreview.mCutStartedAt) * 1000.0,
                                          0.0, (F64)(mPreview.mCutEndMS - mPreview.mCutStartMS));
                    gl_rect_2d(ms_to_x(mPreview.mCutStartMS), wave_top, ms_to_x(mPreview.mCutEndMS), wave_bottom,
                               LLColor4(1.f, 0.9f, 0.25f, 0.10f));
                }

                if (pos_ms >= 0.f)
                {
                    const S32 cx = ms_to_x((U32)pos_ms);
                    gl_rect_2d(cx, wave_top, cx + 2, wave_bottom, LLColor4(1.f, 1.f, 0.3f, 0.95f));
                }
            }
        }

        if (ready)
        {
            const LLRect play = playRect(row);
            const bool playing_this = (row.mID == mPreview.mSound && mPreview.mMode != PREVIEW_NONE);
            gl_rect_2d(play, (i == mHoverRow) ? LLColor4(0.30f, 0.34f, 0.42f, 1.f)
                                              : LLColor4(0.18f, 0.19f, 0.24f, 1.f), true);
            LLUIImagePtr icon = LLUI::getUIImage(playing_this ? "Pause_Off" : "Audio_Off");
            if (icon.notNull())
            {
                icon->draw(play.mLeft + 1, play.mBottom + (play.getHeight() - 14) / 2, 14, 14);
            }
        }
    }

    LLView::draw();
}

// Hand cursor over a preview button.
bool SSSoundAnalysisView::handleHover(S32 x, S32 y, MASK mask)
{
    mHoverRow = playRowAt(x, y);
    getWindow()->setCursor(mHoverRow >= 0 ? UI_CURSOR_HAND : UI_CURSOR_ARROW);
    return true;
}

void SSSoundAnalysisView::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHoverRow = -1;
}

// Preview button clicks only; the rest of the view is opaque filler.
bool SSSoundAnalysisView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    const S32 row = playRowAt(x, y);
    if (row < 0) return false;

    std::vector<RowInfo> rows;
    buildRows(rows);
    togglePreview(rows[row]);
    return true;
}

// Starts or stops a preview of one sound, in the mode the soundscape actually plays it.
// Ambience rows of a multi-sound slot start the slot's sequence there and follow it
// across the assets, wrapping, the way the live loops cycle.
void SSSoundAnalysisView::togglePreview(const RowInfo& row)
{
    const LLUUID& id = row.mID;

    if (mPreview.mMode != PREVIEW_NONE && mPreview.mSound == id)
    {
        fadeOutPreview();
        return;
    }

    fadeOutPreview();

    const SSSoundMeta::Meta* meta = SSSoundMeta::getInstance()->get(id);
    if (!meta || meta->mLengthMS == 0 || !gAudiop) return;

    const F64 now = SSAtmoMagic::getInstance()->sharedTime();
    mPreview.mSound = id;
    mPreview.mQueue.assign(1, id);
    mPreview.mQueueIdx = 0;

    if (row.mPurpose & SSSoundMeta::PURPOSE_STEPS)
    {
        if (step_cut_capable(*meta))
        {
            // Segmentable recording: the gait fires one cut per footfall - preview them on the recording's own cadence.
            mPreview.mMode = PREVIEW_STEPS;
            mPreview.mNextImpactAt = now;
            return;
        }

        // Otherwise the live loop plays the whole recording, resumed mid-phrase from a random onset.
        static LLCachedControl<F32> vol(gSavedSettings, "SSAtmoVolumeFootsteps", 0.5f);
        U32 offset = 0;
        if (meta->mOnsets.size() >= 2)
        {
            const size_t k = (size_t)ll_rand((S32)meta->mOnsets.size() - 1);
            offset = meta->mOnsets[k] + (meta->mOnsets[k + 1] - meta->mOnsets[k]) * 2 / 3;
        }
        mPreview.mMode = PREVIEW_LOOP;
        mPreview.mStartedAt = now;
        mPreview.mStartMS = offset;
        mPreview.mGain = llclamp((F32)vol, 0.f, 1.f);
        mPreview.mSourceID = startSource(id, mPreview.mGain, true, offset);
        return;
    }

    if (row.mPurpose & SSSoundMeta::PURPOSE_TIMING)
    {
        // Thunder: a one-shot of the whole recording, levelled the way updateThunder levels it.
        static LLCachedControl<F32> thunder_vol(gSavedSettings, "SSAtmoVolumeThunder", 2.5f);
        F32 gain = llclamp((F32)thunder_vol, 0.f, 4.f);
        if (meta->mPeakLevel > 0.001f) gain *= llclamp(0.22f / meta->mPeakLevel, 0.5f, 2.f);
        mPreview.mMode = PREVIEW_ONESHOT;
        mPreview.mStartedAt = now;
        mPreview.mEndsAt = now + (F64)meta->mLengthMS / 1000.0;
        mPreview.mGain = llclamp(gain, 0.f, 1.f);
        mPreview.mSourceID = startSource(id, mPreview.mGain, false, 0);
        return;
    }

    // Ambience: loop the one asset, or follow the slot's defined sequence across its
    // sounds starting at the clicked one, mirroring applyLoop's cycling.
    static LLCachedControl<F32> master(gSavedSettings, "SSAtmoVolumeMaster", 0.8f);
    static LLCachedControl<F32> ambient(gSavedSettings, "SSAtmoVolumeAmbient", 1.f);
    mPreview.mGain = llclamp((F32)master * (F32)ambient, 0.f, 1.f);

    for (const SSSoundMeta::SlotInfo& slot : SSSoundMeta::getInstance()->slotsForDebug())
    {
        if (slot.mSource != row.mSource || slot.mSounds.size() < 2) continue;

        S32 start = 0;
        for (S32 i = 0; i < (S32)slot.mSounds.size(); ++i)
        {
            if (slot.mSounds[i] == id) { start = i; break; }
        }
        mPreview.mQueue.clear();
        for (S32 i = 0; i < (S32)slot.mSounds.size(); ++i)
        {
            mPreview.mQueue.push_back(slot.mSounds[(start + i) % (S32)slot.mSounds.size()]);
        }
        break;
    }

    mPreview.mMode = PREVIEW_LOOP;
    if (mPreview.mQueue.size() > 1)
    {
        startSequenceAsset(now);
        return;
    }

    mPreview.mStartedAt = now;
    mPreview.mStartMS = 0;
    mPreview.mSourceID = startSource(id, mPreview.mGain, true, 0);
}

// Fades the current preview out; the reaper cleans the voice up.
void SSSoundAnalysisView::fadeOutPreview()
{
    fadeKill(mPreview.mSourceID, SSAtmoMagic::getInstance()->sharedTime());
    mPreview = Preview();
}

// Hard stop: no fading voice left behind (floater close, failed preview).
void SSSoundAnalysisView::stopPreview()
{
    if (gAudiop)
    {
        if (mPreview.mSourceID.notNull())
        {
            if (LLAudioSource* source = gAudiop->findAudioSource(mPreview.mSourceID))
            {
                gAudiop->cleanupAudioSource(source);
            }
        }
        for (const auto& dying : mDying)
        {
            if (LLAudioSource* source = gAudiop->findAudioSource(dying.first))
            {
                gAudiop->cleanupAudioSource(source);
            }
        }
    }
    mDying.clear();
    mPreview = Preview();
}

// Fires one per-impact cut, mirroring playStepCut: random onset, 60ms pre-roll, cut two thirds to the next onset.
void SSSoundAnalysisView::startCut(F64 when, const SSSoundMeta::Meta& meta)
{
    if (meta.mOnsets.size() < 2 || meta.mImpactRate <= 0.f)
    {
        stopPreview();
        return;
    }

    static LLCachedControl<F32> vol(gSavedSettings, "SSAtmoVolumeFootsteps", 0.5f);

    const size_t k = (size_t)ll_rand((S32)meta.mOnsets.size() - 1);
    const U32 start = (meta.mOnsets[k] > 60) ? meta.mOnsets[k] - 60 : 0;
    const U32 cut = meta.mOnsets[k] + (meta.mOnsets[k + 1] - meta.mOnsets[k]) * 2 / 3;

    mPreview.mCutStartMS = start;
    mPreview.mCutEndMS = cut;
    mPreview.mCutStartedAt = when;
    mPreview.mCutStopAt = when + llclamp((F64)(cut - start) / 1000.0, 0.1, 0.9);
    mPreview.mSourceID = startSource(mPreview.mSound, llclamp((F32)vol, 0.f, 1.f), false, start);
    mPreview.mNextImpactAt = when + 1.0 / (F64)meta.mImpactRate;
}

// Starts the sequence's current asset: one non-looped pass, ending when the recording
// does - the advance deadline the cycle clock reads. Unanalysed assets still play,
// with the engine's own length standing in for the deadline.
void SSSoundAnalysisView::startSequenceAsset(F64 now)
{
    mPreview.mSound = mPreview.mQueue[mPreview.mQueueIdx];
    const SSSoundMeta::Meta* meta = SSSoundMeta::getInstance()->get(mPreview.mSound);
    const F32 len = meta ? (meta->mLengthMS / 1000.f) : ss_sound_length(mPreview.mSound);
    mPreview.mStartedAt = now;
    mPreview.mStartMS = 0;
    mPreview.mEndsAt = now + ((len > 0.f) ? (F64)len : 1.2);
    mPreview.mSourceID = startSource(mPreview.mSound, mPreview.mGain, false, 0);
}

// Drives the preview clock: natural one-shot ends, cut window reaping and the next scheduled footfall.
void SSSoundAnalysisView::updatePreview(F64 now)
{
    if (mPreview.mMode == PREVIEW_NONE) return;

    if (mPreview.mMode == PREVIEW_LOOP && mPreview.mQueue.size() > 1)
    {
        // The sequence cycle: when an asset's pass ends, move to the next in definition
        // order and wrap - the same across-asset loop the live ambience loops run. The
        // meta lookup is skipped: an unanalysed asset in the queue still plays its
        // fallback-length pass, it just draws no playhead.
        if (now >= mPreview.mEndsAt)
        {
            fadeKill(mPreview.mSourceID, now);
            mPreview.mQueueIdx = (mPreview.mQueueIdx + 1) % (S32)mPreview.mQueue.size();
            startSequenceAsset(now);
        }
        return;
    }

    const SSSoundMeta::Meta* meta = SSSoundMeta::getInstance()->get(mPreview.mSound);
    if (!meta)
    {
        stopPreview();
        return;
    }

    if (mPreview.mMode == PREVIEW_ONESHOT)
    {
        if (now >= mPreview.mEndsAt) stopPreview();
        return;
    }

    if (mPreview.mMode == PREVIEW_STEPS)
    {
        // A cut dies at its window edge, exactly as the segment reaper stops it in-world.
        if (mPreview.mSourceID.notNull() && now >= mPreview.mCutStopAt)
        {
            fadeKill(mPreview.mSourceID, now);
            mPreview.mSourceID.setNull();
        }

        if (now >= mPreview.mNextImpactAt)
        {
            if (mPreview.mSourceID.notNull())
            {
                fadeKill(mPreview.mSourceID, now);
                mPreview.mSourceID.setNull();
            }
            startCut(mPreview.mNextImpactAt, *meta);
        }
    }
}

// One preview source at the listener, set up the way the soundscape sets its own voices up.
LLUUID SSSoundAnalysisView::startSource(const LLUUID& sound, F32 gain, bool loop, U32 offset_ms)
{
    if (!gAudiop || sound.isNull()) return LLUUID::null;

    const LLUUID id = LLUUID::generateNewID();
    LLAudioSource* source = new LLAudioSource(id, gAgent.getID(), llclamp(gain, 0.f, 1.f),
                                              LLAudioEngine::AUDIO_TYPE_AMBIENT);
    source->setStartOffsetMS(offset_ms);
    source->setLoop(loop);
    source->setForcedPriority(loop);
    source->setPositionGlobal(gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin()));
    gAudiop->addAudioSource(source);
    source->play(sound);
    SSSoundMeta::getInstance()->fetch(sound);
    return id;
}

// The soundscape's soft stop: silence now, reap shortly after.
void SSSoundAnalysisView::fadeKill(const LLUUID& source_id, F64 now)
{
    if (!gAudiop || source_id.isNull()) return;
    if (LLAudioSource* source = gAudiop->findAudioSource(source_id))
    {
        source->setGain(0.f);
        mDying.emplace_back(source_id, now + 0.06);
    }
}

// Reaps faded-out preview voices.
void SSSoundAnalysisView::cleanupDying(F64 now)
{
    if (!gAudiop)
    {
        mDying.clear();
        return;
    }
    for (size_t i = 0; i < mDying.size(); )
    {
        if (now >= mDying[i].second)
        {
            if (LLAudioSource* source = gAudiop->findAudioSource(mDying[i].first))
            {
                gAudiop->cleanupAudioSource(source);
            }
            mDying.erase(mDying.begin() + i);
        }
        else ++i;
    }
}

// Floater shell; the scrolling analysis view is built in postBuild.
SSFloaterSoundAnalysis::SSFloaterSoundAnalysis(const LLSD& key)
    : LLFloater(key)
{
}

// Creates the analysis view inside the scroll container.
bool SSFloaterSoundAnalysis::postBuild()
{
    LLScrollContainer* scroll = getChild<LLScrollContainer>("analysis_scroll");

    LLView::Params p;
    p.name = "analysis_view";
    p.rect = LLRect(0, 100, scroll->getRect().getWidth() - 16, 0);
    p.mouse_opaque = true;
    mView = new SSSoundAnalysisView(p);
    scroll->addChild(mView);
    return true;
}

// Resizes the inner view to its content height before the normal floater draw.
void SSFloaterSoundAnalysis::draw()
{
    if (mView)
    {
        const S32 needed = llmax(mView->neededHeight(), 100);
        LLScrollContainer* scroll = getChild<LLScrollContainer>("analysis_scroll");
        const S32 want_w = scroll->getRect().getWidth() - 16;
        if (mView->getRect().getHeight() != needed || mView->getRect().getWidth() != want_w)
        {
            mView->reshape(want_w, needed);
        }
    }
    LLFloater::draw();
}

// A closed floater stops drawing, so the preview clock would stall - kill the voices now.
void SSFloaterSoundAnalysis::onClose(bool app_quitting)
{
    if (mView) mView->stopPreview();
}
