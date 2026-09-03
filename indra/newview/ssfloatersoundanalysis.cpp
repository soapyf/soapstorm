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
#include "ssassetlist.h"

#include "llfontgl.h"
#include "llrender2dutils.h"
#include "llscrollcontainer.h"

namespace
{
    const S32 ROW_H = 66;
    const S32 GROUP_H = 20;
    const S32 WAVE_H = 34;
    const S32 PAD = 8;
}

class SSSoundAnalysisView : public LLView
{
public:
    SSSoundAnalysisView(const LLView::Params& p) : LLView(p) {}

    // Content height for the scroll container: a group header per source plus a fixed row per analysed sound.
    S32 neededHeight() const
    {
        S32 rows = 0, groups = 0;
        std::string last_group;
        for (const auto& pair : SSSoundMeta::getInstance()->entriesForDebug())
        {
            if (pair.second.mSource != last_group) { ++groups; last_group = pair.second.mSource; }
            ++rows;
        }
        return groups * GROUP_H + rows * ROW_H + PAD * 2;
    }

    // Renders every READY sound as a stat line plus its envelope waveform with onset/peak/tail markers.
    void draw() override
    {
        const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
        const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
        const S32 width = getRect().getWidth();

        S32 y = getRect().getHeight() - PAD;
        std::string last_group;

        std::vector<std::pair<std::string, const SSSoundMeta::Meta*>> rows;
        std::vector<std::pair<std::string, LLUUID>> keys;
        for (const auto& pair : SSSoundMeta::getInstance()->entriesForDebug())
        {
            if (pair.second.mState != SSSoundMeta::READY) continue;
            keys.emplace_back(pair.second.mSource, pair.first);
        }
        std::sort(keys.begin(), keys.end());

        for (const auto& key : keys)
        {
            const auto& entries = SSSoundMeta::getInstance()->entriesForDebug();
            const SSSoundMeta::Meta& meta = entries.at(key.second).mMeta;

            if (key.first != last_group)
            {
                last_group = key.first;
                y -= GROUP_H;
                bold->renderUTF8(key.first.empty() ? std::string("(unattributed)") : key.first,
                                 0, PAD, y + 5, LLColor4(1.f, 0.85f, 0.4f, 1.f),
                                 LLFontGL::LEFT, LLFontGL::BASELINE);
            }

            y -= ROW_H;

            const std::string name = ss_asset_name(key.second);
            const std::string title = name.empty() ? key.second.asString().substr(0, 12) : name;
            font->renderUTF8(llformat("%s   len %.1fs  onset %.2fs  tail %.1fs  level %.2f  imp/s %.1f  dens %.2f  gap %.2f  cv %.2f  fix %d  crack %.2f",
                                      title.c_str(), meta.mLengthMS / 1000.f, meta.mOnsetMS / 1000.f,
                                      meta.mTailMS / 1000.f, meta.mPeakLevel, meta.mImpactRate, meta.mDensity, meta.mGapFloor, meta.mCadenceCV, (S32)meta.mRepaired, meta.mCrackiness),
                             0, PAD, y + ROW_H - 12, LLColor4(0.9f, 0.9f, 0.9f, 1.f),
                             LLFontGL::LEFT, LLFontGL::BASELINE);

            const S32 wave_top = y + WAVE_H + 6;
            const S32 wave_bottom = y + 6;
            const S32 wave_w = width - PAD * 2;
            gl_rect_2d(PAD, wave_top, PAD + wave_w, wave_bottom, LLColor4(0.07f, 0.07f, 0.09f, 1.f));

            if (!meta.mEnvelope.empty() && meta.mLengthMS > 0)
            {
                const S32 n = (S32)meta.mEnvelope.size();
                for (S32 i = 0; i < n; ++i)
                {
                    const S32 x0 = PAD + i * wave_w / n;
                    const S32 x1 = PAD + (i + 1) * wave_w / n;
                    const S32 h = (S32)(meta.mEnvelope[(size_t)i] * (WAVE_H - 2));
                    gl_rect_2d(x0, wave_bottom + 1 + h, x1, wave_bottom + 1, LLColor4(0.35f, 0.55f, 0.75f, 1.f));
                }

                auto ms_to_x = [&](U32 ms) { return PAD + (S32)((U64)ms * wave_w / meta.mLengthMS); };

                for (U32 ms : meta.mOnsets)
                {
                    const S32 x = ms_to_x(ms);
                    gl_rect_2d(x, wave_bottom + 8, x + 1, wave_bottom + 1, LLColor4(1.f, 1.f, 1.f, 0.7f));
                }

                const S32 px = ms_to_x(meta.mPeakMS);
                gl_rect_2d(px - 1, wave_top, px + 1, wave_bottom, LLColor4(1.f, 0.7f, 0.15f, 0.55f));
                const S32 tx = ms_to_x(meta.mTailMS);
                gl_rect_2d(tx, wave_top, tx + 1, wave_bottom, LLColor4(1.f, 0.25f, 0.2f, 0.9f));
                const S32 ox = ms_to_x(meta.mOnsetMS);
                gl_rect_2d(ox, wave_top, ox + 1, wave_bottom, LLColor4(0.2f, 1.f, 0.35f, 0.95f));
            }
        }

        LLView::draw();
    }
};

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
    p.mouse_opaque = false;
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
