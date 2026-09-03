/**
 * @file ssfloatertexturelist.cpp
 * @brief See ssfloatertexturelist.h.
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

#include "ssfloatertexturelist.h"

#include "llfloaterreg.h"
#include "llinventory.h"
#include "lllocalcliprect.h"
#include "lltextbox.h"
#include "lltooldraganddrop.h"
#include "lltooltip.h"
#include "llui.h"
#include "lluictrlfactory.h"
#include "llviewertexturelist.h"
#include "llwindow.h"

#include <algorithm>

namespace
{
    const S32 SWATCH = 40;
    const S32 ROW_H = SWATCH + 6;
    const S32 ROW_PAD = 2;
    const S32 REMOVE_W = 18;
    const S32 SCROLL_W = 6;

    const S32 DRAG_OUT_M = 46;

    const S32 CHIP_SWATCH = 16;

    // Fetched texture for a thumbnail.
    LLViewerFetchedTexture* ss_texture(const LLUUID& id)
    {
        if (id.isNull()) return NULL;

        LLViewerFetchedTexture* tex = LLViewerTextureManager::getFetchedTexture(
            id, FTT_DEFAULT, true, LLGLTexture::BOOST_PREVIEW);
        if (tex)
        {
            tex->addTextureStats((F32)(SWATCH * SWATCH));
        }
        return tex;
    }

    // Dimension label for a row.
    std::string ss_texture_size(LLViewerFetchedTexture* tex)
    {
        if (!tex) return std::string("--");

        const S32 w = tex->getFullWidth();
        const S32 h = tex->getFullHeight();
        if (w <= 0 || h <= 0) return std::string("loading...");

        return llformat("%d x %d", w, h);
    }
}

static LLDefaultChildRegistry::Register<SSTextureListCtrl> r_ss_texture_list("ss_texture_list");

// Widget params.
SSTextureListCtrl::Params::Params()
:   mode("mode", "random"),
    max_textures("max_textures", 0)
{
}

// Compact strip control showing a texture list; clicking opens the editor.
SSTextureListCtrl::SSTextureListCtrl(const Params& p)
:   LLUICtrl(p),
    mMode(ss_asset_mode_from_key(p.mode)),
    mMaxTextures(llmax(0, p.max_textures()))
{
}

// Draws the thumbnail strip with hover highlight.
void SSTextureListCtrl::draw()
{
    const LLRect& r = getLocalRect();

    gl_rect_2d(r, mHover ? LLColor4(0.19f, 0.20f, 0.25f, 1.f)
                         : LLColor4(0.13f, 0.13f, 0.16f, 1.f), true);
    gl_rect_2d(r, mHover ? LLColor4(0.55f, 0.58f, 0.68f, 1.f)
                         : LLColor4(0.35f, 0.35f, 0.40f, 1.f), false);

    const S32 sw_y = r.mBottom + (r.getHeight() - CHIP_SWATCH) / 2;

    if (!mList.empty())
    {
        if (LLViewerFetchedTexture* tex = ss_texture(mList.front()))
        {
            gl_draw_scaled_image(r.mLeft + 3, sw_y, CHIP_SWATCH, CHIP_SWATCH, tex);
        }
    }
    else
    {
        LLUIImagePtr icon = LLUI::getUIImage("Inv_Texture");
        if (icon.notNull())
        {
            icon->draw(r.mLeft + 3, sw_y, CHIP_SWATCH, CHIP_SWATCH);
        }
    }

    LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const S32 count = (S32)mList.size();

    const std::string label = (count == 0)
        ? std::string("Empty")
        : llformat("%d %s Texture%s", count,
                   (mMode == SS_ASSET_SEQUENCE) ? "Sequenced" : "Random",
                   count == 1 ? "" : "s");

    font->renderUTF8(label, 0, r.mLeft + 3 + CHIP_SWATCH + 5, r.getCenterY(),
                     count ? LLColor4::white : LLColor4(0.6f, 0.6f, 0.6f, 1.f),
                     LLFontGL::LEFT, LLFontGL::VCENTER);

    LLUICtrl::draw();
}

// Clears hover.
void SSTextureListCtrl::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHover = false;
}

// Tracks hover for the highlight.
bool SSTextureListCtrl::handleHover(S32 x, S32 y, MASK mask)
{
    mHover = true;
    getWindow()->setCursor(UI_CURSOR_HAND);
    return true;
}

// Click opens the editor on this list.
bool SSTextureListCtrl::handleMouseDown(S32 x, S32 y, MASK mask)
{
    openEditor();
    return true;
}

// Opens the shared editor floater bound to this control.
void SSTextureListCtrl::openEditor()
{
    SSFloaterTextureList* floater =
        LLFloaterReg::getTypedInstance<SSFloaterTextureList>("ss_texture_list");
    if (!floater) return;

    floater->editFor(this, mSlotLabel);
    floater->openFloater();
    mEditorHandle = floater->getHandle();
}

// Accepts texture drops straight onto the strip.
bool SSTextureListCtrl::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                                          EDragAndDropType cargo_type, void* cargo_data,
                                          EAcceptance* accept, std::string& tooltip_msg)
{
    if (cargo_type != DAD_TEXTURE)
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
        tooltip_msg = llformat("This slot holds at most %d textures", mMaxTextures);
        return true;
    }

    *accept = ACCEPT_YES_SINGLE;

    if (drop)
    {
        mList.push_back(item->getAssetUUID());
        onCommit();
    }

    return true;
}

static LLDefaultChildRegistry::Register<SSTextureListRows> r_ss_texture_rows("ss_texture_list_rows");

// Widget params.
SSTextureListRows::Params::Params()
{
}

// The editor's scrolling row view.
SSTextureListRows::SSTextureListRows(const Params& p)
:   LLUICtrl(p)
{
}

// Total height of all rows.
S32 SSTextureListRows::contentHeight() const
{
    return (S32)mList.size() * (ROW_H + ROW_PAD);
}

// Scroll range.
S32 SSTextureListRows::maxScroll() const
{
    return llmax(0, contentHeight() - getLocalRect().getHeight());
}

// Keeps scroll in range.
void SSTextureListRows::clampScroll()
{
    mScroll = llclamp(mScroll, 0, maxScroll());
}

// Rect of a row.
LLRect SSTextureListRows::rowRect(S32 index) const
{
    const LLRect& r = getLocalRect();
    const S32 top = r.mTop + mScroll - index * (ROW_H + ROW_PAD);
    return LLRect(r.mLeft, top, r.mRight - SCROLL_W, top - ROW_H);
}

// Rect of a row's remove button.
LLRect SSTextureListRows::removeRect(S32 index) const
{
    const LLRect row = rowRect(index);
    return LLRect(row.mRight - REMOVE_W - 4, row.mTop - 4,
                  row.mRight - 4, row.mTop - 4 - REMOVE_W);
}

// Row under a y.
S32 SSTextureListRows::rowAt(S32 y) const
{
    const LLRect& r = getLocalRect();
    const S32 index = (r.mTop + mScroll - y) / (ROW_H + ROW_PAD);
    return (index >= 0 && index < (S32)mList.size()) ? index : -1;
}

// Insertion gap under a y, for drag reordering.
S32 SSTextureListRows::gapAt(S32 y) const
{
    const LLRect& r = getLocalRect();
    const S32 gap = (S32)((F32)(r.mTop + mScroll - y) / (F32)(ROW_H + ROW_PAD) + 0.5f);
    return llclamp(gap, 0, (S32)mList.size());
}

// List changed: clamp scroll and notify.
void SSTextureListRows::changed()
{
    if (mOnChanged) mOnChanged();
}

// Draws rows with thumbnails, names, remove buttons and the drag insertion marker.
void SSTextureListRows::draw()
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
        gl_rect_2d(row, hovered ? LLColor4(0.23f, 0.24f, 0.30f, alpha)
                                : LLColor4(0.16f, 0.16f, 0.20f, alpha), true);

        const LLRect sw(row.mLeft + 3, row.mTop - 3,
                        row.mLeft + 3 + SWATCH, row.mTop - 3 - SWATCH);

        gl_rect_2d(sw, LLColor4(0.f, 0.f, 0.f, alpha), true);

        LLViewerFetchedTexture* tex = ss_texture(mList[i]);
        if (tex)
        {
            gl_draw_scaled_image(sw.mLeft, sw.mBottom, SWATCH, SWATCH, tex,
                                 LLColor4(1.f, 1.f, 1.f, alpha));
        }
        gl_rect_2d(sw, LLColor4(0.35f, 0.35f, 0.40f, alpha), false);

        const std::string name = ss_asset_name(mList[i]);
        const bool named = !name.empty();
        const std::string title = named ? name : mList[i].asString();

        const S32 text_left = sw.mRight + 8;
        const S32 text_right = row.mRight - REMOVE_W - 12;

        font->renderUTF8(title, 0, (F32)text_left, (F32)(row.mTop - 14),
                         named ? LLColor4(0.88f, 0.88f, 0.92f, alpha)
                               : LLColor4(0.62f, 0.62f, 0.68f, alpha),
                         LLFontGL::LEFT, LLFontGL::VCENTER,
                         LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX,
                         llmax(0, text_right - text_left), NULL, true);

        font->renderUTF8(ss_texture_size(tex), 0, (F32)text_left, (F32)(row.mTop - 30),
                         LLColor4(0.60f, 0.62f, 0.68f, alpha),
                         LLFontGL::LEFT, LLFontGL::VCENTER,
                         LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX,
                         llmax(0, text_right - text_left), NULL, true);

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

// Clears hover.
void SSTextureListRows::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHoverRow = -1;
    mHoverRemove = false;
}

// Scrolls the rows.
bool SSTextureListRows::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    if (maxScroll() <= 0) return false;

    mScroll += clicks * (ROW_H + ROW_PAD);
    clampScroll();
    return true;
}

// Full name tooltip per row.
bool SSTextureListRows::handleToolTip(S32 x, S32 y, MASK mask)
{
    const S32 row = rowAt(y);
    if (row >= 0 && row < (S32)mList.size())
    {
        LLToolTipMgr::instance().show(mList[row].asString());
        return true;
    }
    return LLUICtrl::handleToolTip(x, y, mask);
}

// Remove click or drag start.
bool SSTextureListRows::handleMouseDown(S32 x, S32 y, MASK mask)
{
    const S32 row = rowAt(y);
    if (row < 0) return LLUICtrl::handleMouseDown(x, y, mask);

    if (removeRect(row).pointInRect(x, y))
    {
        mList.erase(mList.begin() + row);
        clampScroll();
        changed();
        return true;
    }

    mDragFrom = row;
    mDragTo = row;
    mDragOut = false;
    gFocusMgr.setMouseCapture(this);
    return true;
}

// Hover and drag tracking.
bool SSTextureListRows::handleHover(S32 x, S32 y, MASK mask)
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

// Finishes a drag reorder.
bool SSTextureListRows::handleMouseUp(S32 x, S32 y, MASK mask)
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

    clampScroll();
    changed();
    return true;
}

// Accepts inventory texture drops at the hovered gap.
bool SSTextureListRows::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                                          EDragAndDropType cargo_type, void* cargo_data,
                                          EAcceptance* accept, std::string& tooltip_msg)
{
    if (cargo_type != DAD_TEXTURE)
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
        tooltip_msg = llformat("This slot holds at most %d textures", mMaxTextures);
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

        if (index + 1 >= count && mBatchCount > 1)
        {
            std::sort(mList.begin() + mBatchStart,
                      mList.begin() + mBatchStart + mBatchCount,
                      [](const LLUUID& a, const LLUUID& b)
                      {
                          const std::string na = ss_asset_name(a);
                          const std::string nb = ss_asset_name(b);
                          return ss_natural_less(na.empty() ? a.asString() : na,
                                                 nb.empty() ? b.asString() : nb);
                      });
        }

        mDropGap = -1;
        changed();
    }

    return true;
}

// Editor floater shell.
SSFloaterTextureList::SSFloaterTextureList(const LLSD& key)
:   LLFloater(key)
{
}

// Wires the rows view, the CSV line and the buttons.
bool SSFloaterTextureList::postBuild()
{
    mRows = getChild<SSTextureListRows>("texture_list");

    getChild<LLUICtrl>("csv_editor")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitCsv(); });
    getChild<LLUICtrl>("ok_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onClickOK(); });
    getChild<LLUICtrl>("cancel_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onClickCancel(); });

    mRows->setOnChanged([this]() { refresh(); });
    return true;
}

// Binds the editor to a control, snapshotting the list for cancel.
void SSFloaterTextureList::editFor(SSTextureListCtrl* owner, const std::string& label)
{
    if (!owner) return;

    mOwnerHandle = owner->getHandle();
    mOriginal = owner->getList();
    mMode = owner->getMode();
    mCancelled = false;

    mRows->setList(mOriginal);
    mRows->setMaxTextures(owner->getMaxTextures());

    setTitle(label.empty() ? std::string("Textures") : ("Textures - " + label));

    getChild<LLTextBox>("mode_text")->setText(std::string(
        (mMode == SS_ASSET_SEQUENCE)
            ? "Taken in order"
            : "One taken at random each time"));

    refresh();
}

// Close behaves as cancel.
void SSFloaterTextureList::onClose(bool app_quitting)
{
    if (!mCancelled)
    {
        commitToOwner();
    }
    mCancelled = false;
}

// Pushes the edited list back to the owning control.
void SSFloaterTextureList::commitToOwner()
{
    SSTextureListCtrl* owner = dynamic_cast<SSTextureListCtrl*>(mOwnerHandle.get());
    if (owner && mRows)
    {
        owner->setList(mRows->getList());
        owner->onCommit();
    }
}

// Restores the snapshot on cancel.
void SSFloaterTextureList::restoreOwner()
{
    SSTextureListCtrl* owner = dynamic_cast<SSTextureListCtrl*>(mOwnerHandle.get());
    if (owner)
    {
        owner->setList(mOriginal);
        owner->onCommit();
    }
}

// Parses a pasted CSV into the list.
void SSFloaterTextureList::onCommitCsv()
{
    const std::string csv = getChild<LLUICtrl>("csv_editor")->getValue().asString();
    if (mRows)
    {
        mRows->setList(ss_asset_list_parse(csv));
    }
    refresh();
}

// Rewrites rows and CSV from the list.
void SSFloaterTextureList::refresh()
{
    const S32 count = mRows ? (S32)mRows->getList().size() : 0;

    getChild<LLTextBox>("count_text")->setText(
        (mRows && mRows->isFull()) ? llformat("%d textures (full)", count)
                                   : llformat("%d texture%s", count, count == 1 ? "" : "s"));

    LLUICtrl* csv = getChild<LLUICtrl>("csv_editor");
    if (mRows && !csv->hasFocus())
    {
        csv->setValue(ss_asset_list_str(mRows->getList()));
    }
}

// Commit and close.
void SSFloaterTextureList::onClickOK()
{
    commitToOwner();
    closeFloater();
}

// Restore and close.
void SSFloaterTextureList::onClickCancel()
{
    restoreOwner();
    mCancelled = true;
    closeFloater();
}
