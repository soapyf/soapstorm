/**
 * @file llfloatersnapshot.cpp
 * @brief Snapshot preview window, allowing saving, e-mailing, etc.
 *
 * $LicenseInfo:firstyear=2004&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2016, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llfloatersnapshot.h"

#include "llfloaterreg.h"
#include "llfloaterflickr.h" // <FS:Ansariel> Share to Flickr
#include "fsfloaterprimfeed.h" // <FS:Beq> Share to Primfeed
#include "llimagefiltersmanager.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llpostcard.h"
#include "llresmgr.h"       // LLLocale
#include "llsdserialize.h"
#include "llsidetraypanelcontainer.h"
#include "llsnapshotlivepreview.h"
#include "llspinctrl.h"
#include "llviewercontrol.h"
#include "lltoolfocus.h"
#include "llagentcamera.h"
#include "lltoolmgr.h"
#include "llwebprofile.h"
#include "llviewerdisplay.h"
#include "llfiltereditor.h"
#include "llscrolllistctrl.h"
#include "llavatarnamecache.h"
#include "llcachename.h"
#include "llcharacter.h"
#include "llvoavatar.h"

// <FS:CR> FIRE-9621 - Hide Profile panel on Snapshots on non-sl grids
#ifdef OPENSIM
#include "llviewernetwork.h" // isOpenSim()
#endif // OPENSIM
// </FS:CR>

///----------------------------------------------------------------------------
/// Local function declarations, constants, enums, and typedefs
///----------------------------------------------------------------------------
LLSnapshotFloaterView* gSnapshotFloaterView = nullptr;

constexpr F32 AUTO_SNAPSHOT_TIME_DELAY = 1.f;

constexpr S32 MAX_POSTCARD_DATASIZE = 1572864; // 1.5 megabyte, similar to simulator limit
constexpr S32 MAX_TEXTURE_SIZE = 2048 ; //max upload texture size 2048 * 2048

static LLDefaultChildRegistry::Register<LLSnapshotFloaterView> r("snapshot_floater_view");

// virtual
LLPanelSnapshot* LLFloaterSnapshot::Impl::getActivePanel(LLFloaterSnapshotBase* floater, bool ok_if_not_found)
{
    LLSideTrayPanelContainer* panel_container = floater->getChild<LLSideTrayPanelContainer>("panel_container");
    LLPanelSnapshot* active_panel = dynamic_cast<LLPanelSnapshot*>(panel_container->getCurrentPanel());

    if (!ok_if_not_found)
    {
        if (!active_panel)
        {
            LL_WARNS() << "No snapshot active panel, current panel index: " << panel_container->getCurrentPanelIndex() << LL_ENDL;
        }
        llassert_always(active_panel != NULL);
    }
    return active_panel;
}

// virtual
LLSnapshotModel::ESnapshotType LLFloaterSnapshotBase::ImplBase::getActiveSnapshotType(LLFloaterSnapshotBase* floater)
{
    LLPanelSnapshot* spanel = getActivePanel(floater);

    //return type;
    if (spanel)
    {
        return spanel->getSnapshotType();
    }
    // <FS:Ansariel> Fix XUI parser warnings
    //return LLSnapshotModel::SNAPSHOT_WEB;
    return LLSnapshotModel::SNAPSHOT_NONE;
}

// virtual
LLSnapshotModel::ESnapshotFormat LLFloaterSnapshot::Impl::getImageFormat(LLFloaterSnapshotBase* floater)
{
    LLPanelSnapshot* active_panel = getActivePanel(floater);
    // FIXME: if the default is not PNG, profile uploads may fail.
    return active_panel ? active_panel->getImageFormat() : LLSnapshotModel::SNAPSHOT_FORMAT_PNG;
}

LLSpinCtrl* LLFloaterSnapshot::Impl::getWidthSpinner(LLFloaterSnapshotBase* floater)
{
    LLPanelSnapshot* active_panel = getActivePanel(floater);
    return active_panel ? active_panel->getWidthSpinner() : floater->findChild<LLSpinCtrl>("snapshot_width");
}

LLSpinCtrl* LLFloaterSnapshot::Impl::getHeightSpinner(LLFloaterSnapshotBase* floater)
{
    LLPanelSnapshot* active_panel = getActivePanel(floater);
    return active_panel ? active_panel->getHeightSpinner() : floater->findChild<LLSpinCtrl>("snapshot_height");
}

void LLFloaterSnapshot::Impl::enableAspectRatioCheckbox(LLFloaterSnapshotBase* floater, bool enable)
{
    LLPanelSnapshot* active_panel = getActivePanel(floater);
    if (active_panel)
    {
        active_panel->enableAspectRatioCheckbox(enable);
    }
}

void LLFloaterSnapshot::Impl::setAspectRatioCheckboxValue(LLFloaterSnapshotBase* floater, bool checked)
{
    LLPanelSnapshot* active_panel = getActivePanel(floater);
    if (active_panel)
    {
        active_panel->getChild<LLUICtrl>(active_panel->getAspectRatioCBName())->setValue(checked);
    }
}

LLSnapshotLivePreview* LLFloaterSnapshotBase::getPreviewView()
{
    return impl->getPreviewView();
}

LLSnapshotLivePreview* LLFloaterSnapshotBase::ImplBase::getPreviewView()
{
    LLSnapshotLivePreview* previewp = (LLSnapshotLivePreview*)mPreviewHandle.get();
    return previewp;
}

// virtual
LLSnapshotModel::ESnapshotLayerType LLFloaterSnapshot::Impl::getLayerType(LLFloaterSnapshotBase* floater)
{
    LLSnapshotModel::ESnapshotLayerType type = LLSnapshotModel::SNAPSHOT_TYPE_COLOR;
    LLSD value = floater->getChild<LLUICtrl>("layer_types")->getValue();
    const std::string id = value.asString();
    if (id == "colors")
        type = LLSnapshotModel::SNAPSHOT_TYPE_COLOR;
    else if (id == "depth")
        type = LLSnapshotModel::SNAPSHOT_TYPE_DEPTH;
    // <FS:Ansariel> FIRE-15667: 24bit depth maps
    else if (id == "depth24")
        type = LLSnapshotModel::SNAPSHOT_TYPE_DEPTH24;
    // </FS:Ansariel>
    else if (id == "avatar_transparent")
        type = LLSnapshotModel::SNAPSHOT_TYPE_AVATAR_TRANSPARENT;
    else if (id == "avatar_mask_black")
        type = LLSnapshotModel::SNAPSHOT_TYPE_AVATAR_MASK_BLACK;
    else if (id == "avatar_mask_white")
        type = LLSnapshotModel::SNAPSHOT_TYPE_AVATAR_MASK_WHITE;
    return type;
}

void LLFloaterSnapshot::Impl::setResolution(LLFloaterSnapshotBase* floater, const std::string& comboname)
{
    LLComboBox* combo = floater->getChild<LLComboBox>(comboname);
        combo->setVisible(true);
    updateResolution(combo, floater, false); // to sync spinners with combo
}

//virtual
void LLFloaterSnapshotBase::ImplBase::updateLayout(LLFloaterSnapshotBase* floaterp)
{
    LLSnapshotLivePreview* previewp = getPreviewView();

    //BD - Automatically calculate the size of our snapshot window to enlarge
    //     the snapshot preview to its maximum size, this is especially helpfull
    //     for pretty much every aspect ratio other than 1:1.
    S32 panel_width = llfloor(400.f * gViewerWindow->getWorldViewAspectRatio());

    //BD - Make sure we clamp at 700 here because 700 would be for 16:9 which we
    //     consider the maximum. Everything bigger will be clamped and will have
    //     a slightly smaller preview window which most likely won't fill up the
    //     whole snapshot floater as it should.
    if(panel_width > 700)
    {
        panel_width = 700;
    }

    S32 floater_width{ 224 };
    if(mAdvanced)
    {
        floater_width = floater_width + (S32)panel_width;
    }

    // <FS:Ansariel> Show miniature thumbnail on collapsed snapshot panel
    //LLUICtrl* thumbnail_placeholder = floaterp->getChild<LLUICtrl>("thumbnail_placeholder");
    //thumbnail_placeholder->setVisible(mAdvanced);
    //thumbnail_placeholder->reshape(panel_width, thumbnail_placeholder->getRect().getHeight());
    //floaterp->getChild<LLUICtrl>("image_res_text")->setVisible(advanced);
    //floaterp->getChild<LLUICtrl>("file_size_label")->setVisible(advanced);
    //if (floaterp->hasChild("360_label", true))
    //{
    //  floaterp->getChild<LLUICtrl>("360_label")->setVisible(mAdvanced);
    //}
    //if (!mSkipReshaping)
    //{
    //  thumbnail_placeholder->reshape(panel_width, thumbnail_placeholder->getRect().getHeight());
    //  if (!floaterp->isMinimized())
    //  {
    //      floaterp->reshape(floater_width, floaterp->getRect().getHeight());
    //  }
    //}

    constexpr S32 THUMB_HEIGHT_LARGE = 470;
    constexpr S32 THUMB_HEIGHT_SMALL = 124;
    constexpr S32 THUMB_WIDTH_SMALL = 216;

    previewp->setFixedThumbnailSize(panel_width, mAdvanced ? THUMB_HEIGHT_LARGE : THUMB_HEIGHT_SMALL);
    LLUICtrl* thumbnail_placeholder = floaterp->getChild<LLUICtrl>("thumbnail_placeholder");
    floaterp->getChild<LLUICtrl>("image_res_text")->setVisible(mAdvanced);
    floaterp->getChild<LLUICtrl>("file_size_label")->setVisible(mAdvanced);
    floaterp->getChildView("img_info_border")->setVisible(mAdvanced);
    if (floaterp->hasChild("360_label", true))
    {
        floaterp->getChild<LLUICtrl>("360_label")->setVisible(mAdvanced);
    }
    if (!mSkipReshaping && !floaterp->isMinimized())
    {
        LLView* controls_container = floaterp->getChild<LLView>("controls_container");
        if (mAdvanced)
        {
            LLRect cc_rect = controls_container->getRect();

            floaterp->reshape(floater_width, floaterp->getOriginalHeight());

            controls_container->setRect(cc_rect);
            controls_container->updateBoundingRect();

            thumbnail_placeholder->reshape(panel_width, THUMB_HEIGHT_LARGE);

            LLRect tn_rect = thumbnail_placeholder->getRect();
            tn_rect.setLeftTopAndSize(215, floaterp->getRect().getHeight() - 30, tn_rect.getWidth(), tn_rect.getHeight());
            thumbnail_placeholder->setRect(tn_rect);
            thumbnail_placeholder->updateBoundingRect();

            LLView* info_border = floaterp->getChild<LLView>("img_info_border");
            if (info_border)
            {
                LLRect ib_rect = info_border->getRect();
                S32 border_h = ib_rect.getHeight();
                S32 border_y = tn_rect.mBottom - 5 - border_h;
                ib_rect.setLeftTopAndSize(ib_rect.mLeft, border_y + border_h, ib_rect.getWidth(), border_h);
                info_border->setRect(ib_rect);

                LLView* res_text = floaterp->getChild<LLView>("image_res_text");
                if (res_text)
                {
                    LLRect rt_rect = res_text->getRect();
                    rt_rect.setLeftTopAndSize(rt_rect.mLeft, border_y + border_h - 4, rt_rect.getWidth(), rt_rect.getHeight());
                    res_text->setRect(rt_rect);
                }
                LLView* size_text = floaterp->getChild<LLView>("file_size_label");
                if (size_text)
                {
                    LLRect st_rect = size_text->getRect();
                    st_rect.setLeftTopAndSize(st_rect.mLeft, border_y + border_h - 4, st_rect.getWidth(), st_rect.getHeight());
                    size_text->setRect(st_rect);
                }
                LLView* label_360 = floaterp->getChild<LLView>("360_label");
                if (label_360)
                {
                    LLRect btn_rect = label_360->getRect();
                    btn_rect.setLeftTopAndSize(btn_rect.mLeft, border_y + border_h + 1, btn_rect.getWidth(), btn_rect.getHeight());
                    label_360->setRect(btn_rect);
                }
            }

            previewp->setThumbnailPlaceholderRect(floaterp->getThumbnailPlaceholderRect());
            previewp->setThumbnailImageSize();
        }
        else
        {
            LLRect cc_rect = controls_container->getRect();

            floaterp->reshape(floater_width,floaterp->getOriginalHeight()+THUMB_HEIGHT_SMALL);

            controls_container->setRect(cc_rect);
            controls_container->updateBoundingRect();

            thumbnail_placeholder->reshape(THUMB_WIDTH_SMALL, THUMB_HEIGHT_SMALL);

            LLRect tn_rect = thumbnail_placeholder->getRect();
            tn_rect.setLeftTopAndSize(5, floaterp->getRect().getHeight() - 30, THUMB_WIDTH_SMALL, THUMB_HEIGHT_SMALL);
            thumbnail_placeholder->setRect(tn_rect);
            thumbnail_placeholder->updateBoundingRect();

            previewp->setThumbnailPlaceholderRect(floaterp->getThumbnailPlaceholderRect());
            previewp->setThumbnailImageSize();
        }
    }
    // </FS:Ansariel>

    bool use_freeze_frame = floaterp->mFreezeFrameCheck && floaterp->mFreezeFrameCheck->getValue().asBoolean();

    if (use_freeze_frame)
    {
        // stop all mouse events at fullscreen preview layer
        floaterp->getParent()->setMouseOpaque(true);

        // shrink to smaller layout
        // *TODO: unneeded?
        floaterp->reshape(floaterp->getRect().getWidth(), floaterp->getRect().getHeight());

        // can see and interact with fullscreen preview now
        if (previewp)
        {
            previewp->setVisible(true);
            previewp->setEnabled(true);
        }

        // RN: freeze all avatars
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            floaterp->impl->mAvatarPauseHandles.push_back(character->requestPause());
        }

        // freeze everything else
        gSavedSettings.setBOOL("FreezeTime", true);

        if (LLToolMgr::getInstance()->getCurrentToolset() != gCameraToolset)
        {
            floaterp->impl->mLastToolset = LLToolMgr::getInstance()->getCurrentToolset();
            LLToolMgr::getInstance()->setCurrentToolset(gCameraToolset);
        }
    }
    else // turning off freeze frame mode
    {
        floaterp->getParent()->setMouseOpaque(false);
        // *TODO: unneeded?
        floaterp->reshape(floaterp->getRect().getWidth(), floaterp->getRect().getHeight());
        if (previewp)
        {
            previewp->setVisible(false);
            previewp->setEnabled(false);
        }

        //RN: thaw all avatars
        floaterp->impl->mAvatarPauseHandles.clear();

        // thaw everything else
        gSavedSettings.setBOOL("FreezeTime", false);

        // restore last tool (e.g. pie menu, etc)
        if (floaterp->impl->mLastToolset)
        {
            LLToolMgr::getInstance()->setCurrentToolset(floaterp->impl->mLastToolset);
        }
    }
}

// This is the main function that keeps all the GUI controls in sync with the saved settings.
// It should be called anytime a setting is changed that could affect the controls.
// No other methods should be changing any of the controls directly except for helpers called by this method.
// The basic pattern for programmatically changing the GUI settings is to first set the
// appropriate saved settings and then call this method to sync the GUI with them.
// FIXME: The above comment seems obsolete now.
// virtual
void LLFloaterSnapshot::Impl::updateControls(LLFloaterSnapshotBase* floater)
{
    LLSnapshotModel::ESnapshotType shot_type = getActiveSnapshotType(floater);
    LLSnapshotModel::ESnapshotFormat shot_format = (LLSnapshotModel::ESnapshotFormat)gSavedSettings.getS32("SnapshotFormat");
    LLSnapshotModel::ESnapshotLayerType layer_type = getLayerType(floater);

    floater->getChild<LLComboBox>("local_format_combo")->selectNthItem(gSavedSettings.getS32("SnapshotFormat"));
    floater->getChildView("layer_types")->setEnabled(shot_type == LLSnapshotModel::SNAPSHOT_LOCAL);



    LLPanelSnapshot* active_panel = getActivePanel(floater);
    // <FS:Ansariel> Fix XUI parser warning
    //if (active_panel)
    if (active_panel && active_panel->getName() != "panel_snapshot_options")
    // </FS:Ansariel>
    {
        LLSpinCtrl* width_ctrl = getWidthSpinner(floater);
        LLSpinCtrl* height_ctrl = getHeightSpinner(floater);

        // Initialize spinners.
        // <FS:Ansariel> Store settings at logout; Set in the particular panel classes
        //if (width_ctrl && width_ctrl->getValue().asInteger() == 0)
        //{
        //  S32 w = gViewerWindow->getWindowWidthRaw();
        //  LL_DEBUGS() << "Initializing width spinner (" << width_ctrl->getName() << "): " << w << LL_ENDL;
        //  width_ctrl->setValue(w);
        //  if(getActiveSnapshotType(floater) == LLSnapshotModel::SNAPSHOT_TEXTURE)
        //  {
        //      width_ctrl->setIncrement((F32)(w >> 1));
        //  }
        //}
        //if (height_ctrl && height_ctrl->getValue().asInteger() == 0)
        //{
        //  S32 h = gViewerWindow->getWindowHeightRaw();
        //  LL_DEBUGS() << "Initializing height spinner (" << height_ctrl->getName() << "): " << h << LL_ENDL;
        //  height_ctrl->setValue(h);
        //  if(getActiveSnapshotType(floater) == LLSnapshotModel::SNAPSHOT_TEXTURE)
        //  {
        //      height_ctrl->setIncrement((F32)(h >> 1));
        //  }
        //}
        // </FS:Ansariel>

        // Clamp snapshot resolution to window size when showing UI or HUD in snapshot.
        // <FS:Ansariel> Store settings at logout; Spinners only change for custom resolution
        //if (gSavedSettings.getBOOL("RenderUIInSnapshot") || gSavedSettings.getBOOL("RenderHUDInSnapshot"))
        std::string sdstring = active_panel->getChild<LLComboBox>(active_panel->getImageSizeComboName())->getSelectedValue();
        LLSD sdres;
        std::stringstream sstream(sdstring);
        LLSDSerialize::fromNotation(sdres, sstream, sdstring.size());
        bool is_custom_resolution = (sdres[0].asInteger() == -1 && sdres[1].asInteger() == -1);
        if (is_custom_resolution && (gSavedSettings.getBOOL("RenderUIInSnapshot") || gSavedSettings.getBOOL("RenderHUDInSnapshot")))
        // </FS:Ansariel>
        {
            S32 width = gViewerWindow->getWindowWidthRaw();
            S32 height = gViewerWindow->getWindowHeightRaw();

            width_ctrl->setMaxValue((F32)width);

            height_ctrl->setMaxValue((F32)height);

            if (width_ctrl->getValue().asInteger() > width)
            {
                width_ctrl->forceSetValue(width);
            }
            if (height_ctrl->getValue().asInteger() > height)
            {
                height_ctrl->forceSetValue(height);
            }
        }
        else
        {
            width_ctrl->setMaxValue(MAX_SNAPSHOT_IMAGE_SIZE);
            height_ctrl->setMaxValue(MAX_SNAPSHOT_IMAGE_SIZE);
        }
    }

    LLSnapshotLivePreview* previewp = getPreviewView();
    bool got_bytes = previewp && previewp->getDataSize() > 0;
    bool got_snap = previewp && previewp->getSnapshotUpToDate();

    LL_DEBUGS() << "Is snapshot up-to-date? " << got_snap << LL_ENDL;

    // <FS:Ansariel> Use user-default locale from operating system
    //LLLocale locale(LLLocale::USER_LOCALE);
    LLLocale locale("");
    // </FS:Ansariel>
    std::string bytes_string;
    if (got_snap)
    {
        LLResMgr::getInstance()->getIntegerString(bytes_string, (previewp->getDataSize()) >> 10 );
    }

    // Update displayed image resolution.
    LLTextBox* image_res_tb = floater->getChild<LLTextBox>("image_res_text");
    image_res_tb->setVisible(got_snap);
    if (got_snap)
    {
        image_res_tb->setTextArg("[WIDTH]", llformat("%d", previewp->getEncodedImageWidth()));
        image_res_tb->setTextArg("[HEIGHT]", llformat("%d", previewp->getEncodedImageHeight()));
    }

    LLTextBox* file_size_label = floater->getChild<LLTextBox>("file_size_label");
    file_size_label->setTextArg("[SIZE]", got_snap ? bytes_string : floater->getString("unknown"));

    LLUIColor color = LLUIColorTable::instance().getColor( "LabelTextColor" );
    if (shot_type == LLSnapshotModel::SNAPSHOT_POSTCARD
        && got_bytes
        && previewp->getDataSize() > MAX_POSTCARD_DATASIZE)
    {
        color = LLUIColor(LLColor4::red);
    }
    if (shot_type == LLSnapshotModel::SNAPSHOT_WEB
        && got_bytes
        && previewp->getDataSize() > LLWebProfile::MAX_WEB_DATASIZE)
    {
        color = LLUIColor(LLColor4::red);
    }

    file_size_label->setColor(color);
    file_size_label->setReadOnlyColor(color); // field gets disabled during upload

    // Update the width and height spinners based on the corresponding resolution combos. (?)
    switch(shot_type)
    {
      case LLSnapshotModel::SNAPSHOT_WEB:
        layer_type = LLSnapshotModel::SNAPSHOT_TYPE_COLOR;
        floater->getChild<LLUICtrl>("layer_types")->setValue("colors");
        setResolution(floater, "profile_size_combo");
        break;
      case LLSnapshotModel::SNAPSHOT_POSTCARD:
        layer_type = LLSnapshotModel::SNAPSHOT_TYPE_COLOR;
        floater->getChild<LLUICtrl>("layer_types")->setValue("colors");
        setResolution(floater, "postcard_size_combo");
        break;
      case LLSnapshotModel::SNAPSHOT_TEXTURE:
        layer_type = LLSnapshotModel::SNAPSHOT_TYPE_COLOR;
        floater->getChild<LLUICtrl>("layer_types")->setValue("colors");
        setResolution(floater, "texture_size_combo");
        break;
      case  LLSnapshotModel::SNAPSHOT_LOCAL:
        setResolution(floater, "local_size_combo");
        break;
      default:
        break;
    }
    // <FS:Ansariel> FIRE-16885: Aspect ratio checkbox enabled state sometimes is wrong
    //setAspectRatioCheckboxValue(floater, !floater->impl->mAspectRatioCheckOff && gSavedSettings.getBOOL("KeepAspectForSnapshot"));
    setAspectRatioCheckboxValue(floater, gSavedSettings.getBOOL("KeepAspectForSnapshot"));
    enableAspectRatioCheckbox(floater, !floater->impl->mAspectRatioCheckOff);
    // </FS:Ansariel>

    if (previewp)
    {
        previewp->setSnapshotType(shot_type);
        previewp->setSnapshotFormat(shot_format);
        previewp->setSnapshotBufferType(layer_type);
    }

    LLPanelSnapshot* current_panel = Impl::getActivePanel(floater);
    if (current_panel)
    {
        LLSD info;
        info["have-snapshot"] = got_snap;
        current_panel->updateControls(info);
    }

    // Dynamic Portrait / Landscape orientation button toggle
    S32 cur_w = 0, cur_h = 0;
    if (previewp)
    {
        previewp->getSize(cur_w, cur_h);
    }
    if (cur_w <= 0 || cur_h <= 0)
    {
        LLSpinCtrl* width_ctrl = getWidthSpinner(floater);
        LLSpinCtrl* height_ctrl = getHeightSpinner(floater);
        if (width_ctrl && height_ctrl)
        {
            cur_w = width_ctrl->getValue().asInteger();
            cur_h = height_ctrl->getValue().asInteger();
        }
    }
    if (cur_w <= 0 || cur_h <= 0)
    {
        cur_w = gViewerWindow->getWindowWidthRaw();
        cur_h = gViewerWindow->getWindowHeightRaw();
    }

    bool is_portrait = (cur_h > cur_w);
    // Display active orientation icon with tooltip explaining how to swap.
    const std::string target_icon = is_portrait ? "Icon_Orientation_Portrait" : "Icon_Orientation_Landscape";
    const std::string target_tooltip = is_portrait
        ? "Orientation: Portrait (Click to switch to Landscape)"
        : "Orientation: Landscape (Click to switch to Portrait)";

    LLButton* portrait_btn = floater->findChild<LLButton>("rotate_portrait_btn");
    if (!portrait_btn)
    {
        portrait_btn = floater->findChild<LLButton>("swap_portrait_btn");
    }
    if (portrait_btn)
    {
        portrait_btn->setLabel(LLStringUtil::null);
        portrait_btn->setImageOverlay(target_icon, LLFontGL::HCENTER);
        portrait_btn->setToolTip(target_tooltip);
    }

    if (current_panel)
    {
        LLButton* swap_res_btn = current_panel->findChild<LLButton>("swap_resolution_btn");
        if (swap_res_btn)
        {
            swap_res_btn->setLabel(LLStringUtil::null);
            swap_res_btn->setImageOverlay(target_icon, LLFontGL::HCENTER);
            swap_res_btn->setToolTip(target_tooltip);
        }
    }

    LLButton* popout_btn = floater->findChild<LLButton>("avatar_scope_popout_btn");
    if (popout_btn)
    {
        U32 scope = gSavedSettings.getU32("FSSnapshotAvatarScope");
        popout_btn->setEnabled(scope == SNAPSHOT_AVATARS_SELECTED);
    }

    LL_DEBUGS() << "finished updating controls" << LL_ENDL;
}

//virtual
void LLFloaterSnapshotBase::ImplBase::setStatus(EStatus status, bool ok, const std::string& msg)
{
    switch (status)
    {
    case STATUS_READY:
        setWorking(false);
        setFinished(false);
        break;
    case STATUS_WORKING:
        setWorking(true);
        setFinished(false);
        break;
    case STATUS_FINISHED:
        setWorking(false);
        setFinished(true, ok, msg);
        break;
    }

    mStatus = status;
}

// virtual
void LLFloaterSnapshotBase::ImplBase::setNeedRefresh(bool need)
{
    if (!mFloater) return;

    // Don't display the "Refresh to save" message if we're in auto-refresh mode.
    if (gSavedSettings.getBOOL("AutoSnapshot"))
    {
        need = false;
    }

    mFloater->setRefreshLabelVisible(need);
    mNeedRefresh = need;
}

// virtual
void LLFloaterSnapshotBase::ImplBase::checkAutoSnapshot(LLSnapshotLivePreview* previewp, bool update_thumbnail)
{
    if (previewp)
    {
        bool autosnap = gSavedSettings.getBOOL("AutoSnapshot");
        LL_DEBUGS() << "updating " << (autosnap ? "snapshot" : "thumbnail") << LL_ENDL;
        previewp->updateSnapshot(autosnap, update_thumbnail, autosnap ? AUTO_SNAPSHOT_TIME_DELAY : 0.f);
    }
}

// static
void LLFloaterSnapshotBase::ImplBase::onClickNewSnapshot(void* data)
{
    LLFloaterSnapshotBase* floater = (LLFloaterSnapshotBase *)data;
    LLSnapshotLivePreview* previewp = floater->getPreviewView();
    if (previewp)
    {
        floater->impl->setStatus(ImplBase::STATUS_READY);
        LL_DEBUGS() << "updating snapshot" << LL_ENDL;
        previewp->mForceUpdateSnapshot = true;
    }
}

// static
void LLFloaterSnapshotBase::ImplBase::onClickRotatePortrait(void* data)
{
    LLFloaterSnapshotBase* floater = (LLFloaterSnapshotBase*)data;
    if (!floater || !floater->impl) return;

    LLSnapshotLivePreview* previewp = floater->getPreviewView();
    if (!previewp) return;

    S32 cur_w = 0, cur_h = 0;
    previewp->getSize(cur_w, cur_h);
    if (cur_w <= 0 || cur_h <= 0)
    {
        if (gViewerWindow)
        {
            cur_w = gViewerWindow->getWindowWidthRaw();
            cur_h = gViewerWindow->getWindowHeightRaw();
        }
        else
        {
            cur_w = 1024;
            cur_h = 768;
        }
    }

    S32 new_w = cur_h;
    S32 new_h = cur_w;

    LLPanelSnapshot* active_panel = floater->impl->getActivePanel(floater);
    if (active_panel)
    {
        LLComboBox* combo = active_panel->getImageSizeComboBox();
        std::string target_val = llformat("[i%d,i%d]", new_w, new_h);
        if (combo && combo->getItemByValue(target_val))
        {
            combo->setValue(target_val);
            combo->onCommit();
        }
        else
        {
            if (combo)
            {
                combo->setValue("[i-1,i-1]");
            }
            active_panel->enableAspectRatioCheckbox(true);
            if (active_panel->getWidthSpinner())
            {
                active_panel->getWidthSpinner()->setEnabled(true);
                active_panel->getWidthSpinner()->setValue(new_w);
            }
            if (active_panel->getHeightSpinner())
            {
                active_panel->getHeightSpinner()->setEnabled(true);
                active_panel->getHeightSpinner()->setValue(new_h);
            }
            gSavedSettings.setS32("LastSnapshotToDiskWidth", new_w);
            gSavedSettings.setS32("LastSnapshotToDiskHeight", new_h);

            LLSD info;
            info["w"] = new_w;
            info["h"] = new_h;
            floater->notify(LLSD().with("custom-res-change", info));
        }
    }
    else
    {
        previewp->setSize(new_w, new_h);
        floater->impl->setStatus(ImplBase::STATUS_READY);
        previewp->mForceUpdateSnapshot = true;
        previewp->updateSnapshot(true);
    }
    floater->impl->updateControls(floater);
}

// static
void LLFloaterSnapshotBase::ImplBase::onClickAutoSnap(LLUICtrl *ctrl, void* data)
{
    LLCheckBoxCtrl *check = (LLCheckBoxCtrl *)ctrl;
    gSavedSettings.setBOOL( "AutoSnapshot", check->get() );

    LLFloaterSnapshotBase *view = (LLFloaterSnapshotBase *)data;
    if (view)
    {
        view->impl->checkAutoSnapshot(view->getPreviewView());
        view->impl->updateControls(view);
    }
}

// static
void LLFloaterSnapshotBase::ImplBase::onClickNoPost(LLUICtrl *ctrl, void* data)
{
    bool no_post = ((LLCheckBoxCtrl*)ctrl)->get();
    gSavedSettings.setBOOL("RenderSnapshotNoPost", no_post);

    LLFloaterSnapshotBase* view = (LLFloaterSnapshotBase*)data;
    view->getPreviewView()->updateSnapshot(true, true);
    view->impl->updateControls(view);
}

// static
void LLFloaterSnapshotBase::ImplBase::onClickFilter(LLUICtrl *ctrl, void* data)
{
    LLFloaterSnapshotBase *view = (LLFloaterSnapshotBase *)data;
    if (view)
    {
        view->impl->updateControls(view);
        LLSnapshotLivePreview* previewp = view->getPreviewView();
        if (previewp)
        {
            view->impl->checkAutoSnapshot(previewp);
            // Note : index 0 of the filter drop down is assumed to be "No filter" in whichever locale
            LLComboBox* filterbox = static_cast<LLComboBox *>(view->getChild<LLComboBox>("filters_combobox"));
            std::string filter_name = (filterbox->getCurrentIndex() ? filterbox->getSimple() : "");
            previewp->setFilter(filter_name);
            previewp->updateSnapshot(true);
        }
    }
}

// static
void LLFloaterSnapshotBase::ImplBase::onClickDisplaySetting(LLUICtrl* ctrl, void* data)
{
    LLFloaterSnapshot* view = (LLFloaterSnapshot*)data;
    if (view)
    {
        U32 scope = gSavedSettings.getU32("FSSnapshotAvatarScope");
        gSavedSettings.setBOOL("FSSnapshotCaptureAllAvatars", (scope == SNAPSHOT_AVATARS_ALL));

        if (scope == SNAPSHOT_AVATARS_SELECTED)
        {
            LLFloaterSnapshotAvatarSelect* picker = LLFloaterReg::showTypedInstance<LLFloaterSnapshotAvatarSelect>("snapshot_avatar_select");
            if (picker)
            {
                picker->positionBeside(view);
                view->addDependentFloater(picker);
                picker->refreshList();
            }
        }
        else
        {
            LLFloaterSnapshotAvatarSelect* picker = LLFloaterReg::findTypedInstance<LLFloaterSnapshotAvatarSelect>("snapshot_avatar_select");
            if (picker && picker->isInVisibleChain())
            {
                picker->closeFloater();
            }
        }

        LLSnapshotLivePreview* previewp = view->getPreviewView();
        if (previewp)
        {
            previewp->updateSnapshot(true, true);
        }
        view->impl->updateControls(view);
    }
}

// static
void LLFloaterSnapshotBase::ImplBase::onClickAvatarScopePopout(void* data)
{
    LLFloaterSnapshot* view = (LLFloaterSnapshot*)data;
    if (!view) return;

    LLFloaterSnapshotAvatarSelect* picker = LLFloaterReg::showTypedInstance<LLFloaterSnapshotAvatarSelect>("snapshot_avatar_select");
    if (picker)
    {
        picker->positionBeside(view);
        view->addDependentFloater(picker);
        picker->refreshList();
    }
}

// <FS:Ansariel> FIRE-15853: HUDs, interface or L$ balance checkbox don't update actual screenshot image
// static
void LLFloaterSnapshotBase::ImplBase::onClickCurrencyCheck(LLUICtrl *ctrl, void* data)
{
    LLFloaterSnapshot *view = (LLFloaterSnapshot *)data;
    if (view)
    {
        LLSnapshotLivePreview* previewp = view->getPreviewView();
        if (previewp)
        {
            previewp->updateSnapshot(true, true);
        }
        view->impl->updateControls(view);
    }
}
// </FS:Ansariel>

void LLFloaterSnapshot::Impl::applyKeepAspectCheck(LLFloaterSnapshotBase* view, bool checked)
{
    gSavedSettings.setBOOL("KeepAspectForSnapshot", checked);

    if (view)
    {
        LLPanelSnapshot* active_panel = getActivePanel(view);
        if (checked && active_panel)
        {
            LLComboBox* combo = view->getChild<LLComboBox>(active_panel->getImageSizeComboName());
            combo->setCurrentByIndex(combo->getItemCount() - 1); // "custom" is always the last index
        }

        LLSnapshotLivePreview* previewp = getPreviewView() ;
        if(previewp)
        {
            previewp->mKeepAspectRatio = gSavedSettings.getBOOL("KeepAspectForSnapshot") ;

            S32 w, h ;
            previewp->getSize(w, h) ;
            updateSpinners(view, previewp, w, h, true); // may change w and h

            LL_DEBUGS() << "updating thumbnail" << LL_ENDL;
            previewp->setSize(w, h) ;
            previewp->updateSnapshot(true);
            checkAutoSnapshot(previewp, true);
        }
    }
}

// static
void LLFloaterSnapshotBase::ImplBase::onCommitFreezeFrame(LLUICtrl* ctrl, void* data)
{
    LLCheckBoxCtrl* check_box = (LLCheckBoxCtrl*)ctrl;
    LLFloaterSnapshotBase *view = (LLFloaterSnapshotBase *)data;
    LLSnapshotLivePreview* previewp = view->getPreviewView();

    if (!view || !check_box || !previewp)
    {
        return;
    }

    gSavedSettings.setBOOL("UseFreezeFrame", check_box->get());

    if (check_box->get())
    {
        previewp->prepareFreezeFrame();
    }

    view->impl->updateLayout(view);
}

void LLFloaterSnapshot::Impl::checkAspectRatio(LLFloaterSnapshotBase *view, S32 index)
{
    LLSnapshotLivePreview *previewp = getPreviewView() ;

    // Don't round texture sizes; textures are commonly stretched in world, profiles, etc and need to be "squashed" during upload, not cropped here
    if (LLSnapshotModel::SNAPSHOT_TEXTURE == getActiveSnapshotType(view))
    {
        previewp->mKeepAspectRatio = false ;
        return ;
    }

    bool keep_aspect = false, enable_cb = false;

    if (0 == index) // current window size
    {
        enable_cb = false;
        keep_aspect = true;
    }
    else if (-1 == index) // custom
    {
        enable_cb = true;
        keep_aspect = gSavedSettings.getBOOL("KeepAspectForSnapshot");
    }
    else // predefined resolution
    {
        enable_cb = false;
        keep_aspect = false;
    }

    view->impl->mAspectRatioCheckOff = !enable_cb;

    if (previewp)
    {
        previewp->mKeepAspectRatio = keep_aspect;
    }
}

// Show/hide upload progress indicators.
void LLFloaterSnapshotBase::ImplBase::setWorking(bool working)
{
    LLUICtrl* working_lbl = mFloater->getChild<LLUICtrl>("working_lbl");
    working_lbl->setVisible(working);
    mFloater->getChild<LLUICtrl>("working_indicator")->setVisible(working);

    // All controls should be disabled while posting.
    mFloater->setCtrlsEnabled(!working);
    if (LLPanelSnapshot* active_panel = getActivePanel(mFloater))
    {
        active_panel->enableControls(!working);
        if (working)
        {
            const std::string panel_name = active_panel->getName();
            const std::string prefix = panel_name.substr(getSnapshotPanelPrefix().size());
            std::string progress_text = mFloater->getString(prefix + "_" + "progress_str");
            working_lbl->setValue(progress_text);
        }
    }
}

//virtual
std::string LLFloaterSnapshot::Impl::getSnapshotPanelPrefix()
{
    return "panel_snapshot_";
}

// Show/hide upload status message.
// virtual
void LLFloaterSnapshot::Impl::setFinished(bool finished, bool ok, const std::string& msg)
{
    mFloater->setSuccessLabelPanelVisible(finished && ok);
    mFloater->setFailureLabelPanelVisible(finished && !ok);

    if (finished)
    {
        LLUICtrl* finished_lbl = mFloater->getChild<LLUICtrl>(ok ? "succeeded_lbl" : "failed_lbl");
        std::string result_text = mFloater->getString(msg + "_" + (ok ? "succeeded_str" : "failed_str"));
        finished_lbl->setValue(result_text);
        // <FS:Ansariel> Don't return to target selection after taking a snapshot
        LLPanelSnapshot* panel = getActivePanel(mFloater);
        if (panel)
        {
            std::string sdstring = panel->getImageSizeComboBox()->getSelectedValue();
            LLSD sdres;
            std::stringstream sstream(sdstring);
            LLSDSerialize::fromNotation(sdres, sstream, sdstring.size());
            bool is_custom_resolution = (sdres[0].asInteger() == -1 && sdres[1].asInteger() == -1);

            panel->enableAspectRatioCheckbox(is_custom_resolution);
            panel->getWidthSpinner()->setEnabled(is_custom_resolution);
            panel->getHeightSpinner()->setEnabled(is_custom_resolution);
        }
        // </FS:Ansariel>
    }
}

// Apply a new resolution selected from the given combobox.
void LLFloaterSnapshot::Impl::updateResolution(LLUICtrl* ctrl, void* data, bool do_update)
{
    LLComboBox* combobox = (LLComboBox*)ctrl;
    LLFloaterSnapshot *view = (LLFloaterSnapshot *)data;

    if (!view || !combobox)
    {
        llassert(view && combobox);
        return;
    }

    std::string sdstring = combobox->getSelectedValue();
    LLSD sdres;
    std::stringstream sstream(sdstring);
    LLSDSerialize::fromNotation(sdres, sstream, sdstring.size());

    S32 width = sdres[0];
    S32 height = sdres[1];

    if (width < -1 || height < -1)
    {
        return;
    }

    // <FS:Ansariel> Store settings at logout
    bool is_custom_resolution = (width == -1 && height == -1);

    LLSnapshotLivePreview* previewp = getPreviewView();
    if (previewp && combobox->getCurrentIndex() >= 0)
    {
        S32 original_width = 0, original_height = 0;
        previewp->getSize(original_width, original_height);

        if (gSavedSettings.getBOOL("RenderUIInSnapshot") || gSavedSettings.getBOOL("RenderHUDInSnapshot"))
        { //clamp snapshot resolution to window size when showing UI or HUD in snapshot
            width = llmin(width, gViewerWindow->getWindowWidthRaw());
            height = llmin(height, gViewerWindow->getWindowHeightRaw());
        }

        if (width == 0 || height == 0)
        {
            // take resolution from current window size
            LL_DEBUGS() << "Setting preview res from window: " << gViewerWindow->getWindowWidthRaw() << "x" << gViewerWindow->getWindowHeightRaw() << LL_ENDL;
            previewp->setSize(gViewerWindow->getWindowWidthRaw(), gViewerWindow->getWindowHeightRaw());
        }
        else if (width == -1 || height == -1)
        {
            // load last custom value
            S32 new_width = 0, new_height = 0;
            LLPanelSnapshot* spanel = getActivePanel(view);
            if (spanel)
            {
                LL_DEBUGS() << "Loading typed res from panel " << spanel->getName() << LL_ENDL;
                new_width = spanel->getTypedPreviewWidth();
                new_height = spanel->getTypedPreviewHeight();

                // Limit custom size for inventory snapshots to 2048x2048 px.
                if (getActiveSnapshotType(view) == LLSnapshotModel::SNAPSHOT_TEXTURE)
                {
                    new_width = llmin(new_width, MAX_TEXTURE_SIZE);
                    new_height = llmin(new_height, MAX_TEXTURE_SIZE);
                }
            }
            else
            {
                LL_DEBUGS() << "No custom res chosen, setting preview res from window: "
                    << gViewerWindow->getWindowWidthRaw() << "x" << gViewerWindow->getWindowHeightRaw() << LL_ENDL;
                new_width = gViewerWindow->getWindowWidthRaw();
                new_height = gViewerWindow->getWindowHeightRaw();
            }

            llassert(new_width > 0 && new_height > 0);
            previewp->setSize(new_width, new_height);
        }
        else
        {
            // use the resolution from the selected pre-canned drop-down choice
            LL_DEBUGS() << "Setting preview res selected from combo: " << width << "x" << height << LL_ENDL;
            previewp->setSize(width, height);
        }

        checkAspectRatio(view, width);

        previewp->getSize(width, height);

        // <FS:Ansariel> Store settings at logout; Only update spinners when using custom resolution
        if (is_custom_resolution)
        {
        // </FS:Ansariel>
        LLSpinCtrl* height_ctrl = getHeightSpinner(view);
        if (height_ctrl)
        {
            // We use the height spinner here because we come here via the aspect ratio
            // checkbox as well and we want height always changing to width by default.
            // If we use the width spinner we would change width according to height by
            // default, that is not what we want.
            updateSpinners(view, previewp, width, height, !height_ctrl->isDirty()); // may change width and height
        }

        LLSpinCtrl* width_ctrl = getWidthSpinner(view);
        if (width_ctrl
            && height_ctrl
            && (width_ctrl->getValue().asInteger() != width || height_ctrl->getValue().asInteger() != height))
        {
            width_ctrl->setValue(width);
            height_ctrl->setValue(height);
            if (getActiveSnapshotType(view) == LLSnapshotModel::SNAPSHOT_TEXTURE)
            {
                width_ctrl->setIncrement((F32)(width >> 1));
                height_ctrl->setIncrement((F32)(height >> 1));
            }
        }
        // <FS:Ansariel> Store settings at logout; Only update spinners when using custom resolution
        }

        getWidthSpinner(view)->setEnabled(is_custom_resolution);
        getHeightSpinner(view)->setEnabled(is_custom_resolution);
        // </FS:Ansariel>

        if(original_width != width || original_height != height)
        {
            previewp->setSize(width, height);

            // hide old preview as the aspect ratio could be wrong
            checkAutoSnapshot(previewp, false);
            LL_DEBUGS() << "updating thumbnail" << LL_ENDL;
            // Don't update immediately, give window chance to redraw
            getPreviewView()->updateSnapshot(true, false, 1.f);
            if(do_update)
            {
                LL_DEBUGS() << "Will update controls" << LL_ENDL;
                updateControls(view);
            }
        }
    }
}

// static
void LLFloaterSnapshot::Impl::onCommitLayerTypes(LLUICtrl* ctrl, void*data)
{
    LLComboBox* combobox = (LLComboBox*)ctrl;

    LLFloaterSnapshot *view = (LLFloaterSnapshot *)data;

    if (view)
    {
        LLSnapshotLivePreview* previewp = view->getPreviewView();
        LLSnapshotModel::ESnapshotLayerType layer_type = view->impl->getLayerType(view);
        if (previewp)
        {
            previewp->setSnapshotBufferType(layer_type);
        }
        if (layer_type == LLSnapshotModel::SNAPSHOT_TYPE_AVATAR_TRANSPARENT)
        {
            LLComboBox* format_combo = view->findChild<LLComboBox>("local_format_combo");
            if (format_combo && format_combo->getCurrentIndex() != LLSnapshotModel::SNAPSHOT_FORMAT_PNG)
            {
                format_combo->selectNthItem(LLSnapshotModel::SNAPSHOT_FORMAT_PNG);
                gSavedSettings.setS32("SnapshotFormat", LLSnapshotModel::SNAPSHOT_FORMAT_PNG);
                if (previewp)
                {
                    previewp->setSnapshotFormat(LLSnapshotModel::SNAPSHOT_FORMAT_PNG);
                }
            }
        }
        view->impl->checkAutoSnapshot(previewp, true);
        previewp->updateSnapshot(true, true);
        view->impl->updateControls(view);
    }
}

void LLFloaterSnapshot::Impl::onImageQualityChange(LLFloaterSnapshotBase* view, S32 quality_val)
{
    LLSnapshotLivePreview* previewp = getPreviewView();
    if (previewp)
    {
        previewp->setSnapshotQuality(quality_val);
    }
}

void LLFloaterSnapshot::Impl::onImageFormatChange(LLFloaterSnapshotBase* view)
{
    if (view)
    {
        gSavedSettings.setS32("SnapshotFormat", getImageFormat(view));
        LL_DEBUGS() << "image format changed, updating snapshot" << LL_ENDL;
        getPreviewView()->updateSnapshot(true);
        updateControls(view);
    }
}

// Sets the named size combo to "custom" mode.
void LLFloaterSnapshot::Impl::comboSetCustom(LLFloaterSnapshotBase* floater, const std::string& comboname)
{
    LLComboBox* combo = floater->getChild<LLComboBox>(comboname);
    combo->setCurrentByIndex(combo->getItemCount() - 1); // "custom" is always the last index
    checkAspectRatio(floater, -1); // -1 means custom
}

// Update supplied width and height according to the constrain proportions flag; limit them by max_val.
bool LLFloaterSnapshot::Impl::checkImageSize(LLSnapshotLivePreview* previewp, S32& width, S32& height, bool isWidthChanged, S32 max_value) const
{
    S32 w = width ;
    S32 h = height ;

    if(previewp && previewp->mKeepAspectRatio)
    {
        if(gViewerWindow->getWindowWidthRaw() < 1 || gViewerWindow->getWindowHeightRaw() < 1)
        {
            return false ;
        }

        //aspect ratio of the current window
        F32 aspect_ratio = (F32)gViewerWindow->getWindowWidthRaw() / gViewerWindow->getWindowHeightRaw() ;

        //change another value proportionally
        if(isWidthChanged)
        {
            height = ll_round(width / aspect_ratio) ;
        }
        else
        {
            width = ll_round(height * aspect_ratio) ;
        }

        //bound w/h by the max_value
        if(width > max_value || height > max_value)
        {
            if(width > height)
            {
                width = max_value ;
                height = (S32)(width / aspect_ratio) ;
            }
            else
            {
                height = max_value ;
                width = (S32)(height * aspect_ratio) ;
            }
        }
    }

    return (w != width || h != height) ;
}

void LLFloaterSnapshot::Impl::setImageSizeSpinnersValues(LLFloaterSnapshotBase* view, S32 width, S32 height)
{
    LLSpinCtrl* width_ctrl = getWidthSpinner(view);
    LLSpinCtrl* height_ctrl = getHeightSpinner(view);
    if (!height_ctrl || !width_ctrl)
    {
        return;
    }
    width_ctrl->forceSetValue(width);
    height_ctrl->forceSetValue(height);
    if (getActiveSnapshotType(view) == LLSnapshotModel::SNAPSHOT_TEXTURE)
    {
        width_ctrl->setIncrement((F32)(width >> 1));
        height_ctrl->setIncrement((F32)(height >> 1));
    }
}

void LLFloaterSnapshot::Impl::updateSpinners(LLFloaterSnapshotBase* view, LLSnapshotLivePreview* previewp, S32& width, S32& height, bool is_width_changed)
{
    LLSpinCtrl* width_ctrl = getWidthSpinner(view);
    LLSpinCtrl* height_ctrl = getHeightSpinner(view);
    if (!height_ctrl || !width_ctrl)
    {
        return;
    }
    width_ctrl->resetDirty();
    height_ctrl->resetDirty();
    if (checkImageSize(previewp, width, height, is_width_changed, previewp->getMaxImageSize()))
    {
        setImageSizeSpinnersValues(view, width, height);
    }
}

void LLFloaterSnapshot::Impl::applyCustomResolution(LLFloaterSnapshotBase* view, S32 w, S32 h)
{
    LL_DEBUGS() << "applyCustomResolution(" << w << ", " << h << ")" << LL_ENDL;
    if (!view) return;

    LLSnapshotLivePreview* previewp = getPreviewView();
    if (previewp)
    {
        S32 curw,curh;
        previewp->getSize(curw, curh);

        if (w != curw || h != curh)
        {
            //if to upload a snapshot, process spinner input in a special way.
            LLSpinCtrl* width_ctrl = getWidthSpinner(view);
            if (width_ctrl)
            {
                previewp->setMaxImageSize((S32)width_ctrl->getMaxValue());
            }
            else
            {
                previewp->setMaxImageSize((S32)2048);
            }

            previewp->setSize(w,h);
            checkAutoSnapshot(previewp, false);
            // <FS:Ansariel> Store settings at logout
            //comboSetCustom(view, "profile_size_combo");
            //comboSetCustom(view, "postcard_size_combo");
            //comboSetCustom(view, "texture_size_combo");
            //comboSetCustom(view, "local_size_combo");
            // </FS:Ansariel>
            LL_DEBUGS() << "applied custom resolution, updating thumbnail" << LL_ENDL;
            previewp->updateSnapshot(true);
            updateControls(view);
        }
    }
}

// static
void LLFloaterSnapshot::Impl::onSnapshotUploadFinished(LLFloaterSnapshotBase* floater, bool status)
{
    floater->impl->setStatus(STATUS_FINISHED, status, "profile");
}

// static
void LLFloaterSnapshot::Impl::onSendingPostcardFinished(LLFloaterSnapshotBase* floater, bool status)
{
    floater->impl->setStatus(STATUS_FINISHED, status, "postcard");
}

///----------------------------------------------------------------------------
/// Class LLFloaterSnapshotBase
///----------------------------------------------------------------------------

// Default constructor
LLFloaterSnapshotBase::LLFloaterSnapshotBase(const LLSD& key)
    : LLFloater(key),
      mRefreshBtn(NULL),
      mRefreshLabel(NULL),
      mSucceessLblPanel(NULL),
      mFailureLblPanel(NULL)
{
}

LLFloaterSnapshotBase::~LLFloaterSnapshotBase()
{
    if (impl->mPreviewHandle.get()) impl->mPreviewHandle.get()->die();

    //unfreeze everything else
    gSavedSettings.setBOOL("FreezeTime", false);

    if (impl->mLastToolset)
    {
        LLToolMgr::getInstance()->setCurrentToolset(impl->mLastToolset);
    }

    delete impl;
}

///----------------------------------------------------------------------------
/// Class LLFloaterSnapshot
///----------------------------------------------------------------------------

// Default constructor
LLFloaterSnapshot::LLFloaterSnapshot(const LLSD& key)
    : LLFloaterSnapshotBase(key)
    , mIsOpen(false) // <FS:Ansariel> FIRE-16145: CTRL-SHIFT-S doesn't update the snapshot anymore
{
    impl = new Impl(this);
}

LLFloaterSnapshot::~LLFloaterSnapshot()
{
}

// virtual
bool LLFloaterSnapshot::postBuild()
{
    mRefreshBtn = getChild<LLUICtrl>("new_snapshot_btn");
    childSetAction("new_snapshot_btn", ImplBase::onClickNewSnapshot, this);
    if (hasChild("rotate_portrait_btn", true))
    {
        childSetAction("rotate_portrait_btn", ImplBase::onClickRotatePortrait, this);
    }
    if (hasChild("swap_portrait_btn", true))
    {
        childSetAction("swap_portrait_btn", ImplBase::onClickRotatePortrait, this);
    }
    mRefreshLabel = getChild<LLUICtrl>("refresh_lbl");
    mSucceessLblPanel = getChild<LLUICtrl>("succeeded_panel");
    mFailureLblPanel = getChild<LLUICtrl>("failed_panel");

    childSetCommitCallback("ui_check", ImplBase::onClickDisplaySetting, this);
    childSetCommitCallback("balance_check", ImplBase::onClickDisplaySetting, this);
    childSetCommitCallback("hud_check", ImplBase::onClickDisplaySetting, this);
    if (hasChild("avatar_scope_combo", true))
    {
        childSetCommitCallback("avatar_scope_combo", ImplBase::onClickDisplaySetting, this);
    }
    if (hasChild("avatar_scope_popout_btn", true))
    {
        childSetAction("avatar_scope_popout_btn", ImplBase::onClickAvatarScopePopout, this);
    }
    if (hasChild("capture_all_avatars_check", true))
    {
        childSetCommitCallback("capture_all_avatars_check", ImplBase::onClickDisplaySetting, this);
    }

    if (gSavedSettings.getU32("FSSnapshotAvatarScope") == SNAPSHOT_AVATARS_SELECTED)
    {
        LLFloaterSnapshotAvatarSelect* picker = LLFloaterReg::showTypedInstance<LLFloaterSnapshotAvatarSelect>("snapshot_avatar_select");
        if (picker)
        {
            picker->positionBeside(this);
            addDependentFloater(picker);
            picker->refreshList();
        }
    }

    // <FS:Ansariel> FIRE-15853: HUDs, interface or L$ balance checkbox don't update actual screenshot image
    childSetCommitCallback("currency_check", ImplBase::onClickCurrencyCheck, this);

    ((Impl*)impl)->setAspectRatioCheckboxValue(this, gSavedSettings.getBOOL("KeepAspectForSnapshot"));

    childSetCommitCallback("layer_types", Impl::onCommitLayerTypes, this);
    getChild<LLUICtrl>("layer_types")->setValue("colors");
    getChildView("layer_types")->setEnabled(false);

    mFreezeFrameCheck = getChild<LLUICtrl>("freeze_frame_check");
    mFreezeFrameCheck->setValue(gSavedSettings.getBOOL("UseFreezeFrame"));
    mFreezeFrameCheck->setCommitCallback(&ImplBase::onCommitFreezeFrame, this);

    getChild<LLUICtrl>("auto_snapshot_check")->setValue(gSavedSettings.getBOOL("AutoSnapshot"));
    childSetCommitCallback("auto_snapshot_check", ImplBase::onClickAutoSnap, this);

    getChild<LLUICtrl>("no_post_check")->setValue(gSavedSettings.getBOOL("RenderSnapshotNoPost"));
    childSetCommitCallback("no_post_check", ImplBase::onClickNoPost, this);

    getChild<LLButton>("retract_btn")->setCommitCallback(boost::bind(&LLFloaterSnapshot::onExtendFloater, this));
    getChild<LLButton>("extend_btn")->setCommitCallback(boost::bind(&LLFloaterSnapshot::onExtendFloater, this));

    // <FS:Ansariel> Better 360 snapshot button
    //getChild<LLTextBox>("360_label")->setSoundFlags(LLView::MOUSE_UP);
    //getChild<LLTextBox>("360_label")->setShowCursorHand(false);
    //getChild<LLTextBox>("360_label")->setClickedCallback(boost::bind(&LLFloaterSnapshot::on360Snapshot, this));
    getChild<LLButton>("360_label")->setCommitCallback(boost::bind(&LLFloaterSnapshot::on360Snapshot, this));
    // </FS:Ansariel>

    // Filters
    LLComboBox* filterbox = getChild<LLComboBox>("filters_combobox");
    std::vector<std::string> filter_list = LLImageFiltersManager::getInstance()->getFiltersList();
    for (U32 i = 0; i < filter_list.size(); i++)
    {
        filterbox->add(filter_list[i]);
    }
    childSetCommitCallback("filters_combobox", ImplBase::onClickFilter, this);

    LLWebProfile::setImageUploadResultCallback(boost::bind(&Impl::onSnapshotUploadFinished, this, _1));
    LLPostCard::setPostResultCallback(boost::bind(&Impl::onSendingPostcardFinished, this, _1));

    mThumbnailPlaceholder = getChild<LLUICtrl>("thumbnail_placeholder");

    // create preview window
    LLRect full_screen_rect = getRootView()->getRect();
    LLSnapshotLivePreview::Params p;
    p.rect(full_screen_rect);
    LLSnapshotLivePreview* previewp = new LLSnapshotLivePreview(p);
    LLView* parent_view = gSnapshotFloaterView->getParent();

    parent_view->removeChild(gSnapshotFloaterView);
    // make sure preview is below snapshot floater
    parent_view->addChild(previewp);
    parent_view->addChild(gSnapshotFloaterView);

    //move snapshot floater to special purpose snapshotfloaterview
    gFloaterView->removeChild(this);
    gSnapshotFloaterView->addChild(this);

    // Pre-select "Current Window" resolution.
    // <FS:Ansariel> Store settings at logout
    //getChild<LLComboBox>("profile_size_combo")->selectNthItem(0);
    //getChild<LLComboBox>("postcard_size_combo")->selectNthItem(0);
    //getChild<LLComboBox>("texture_size_combo")->selectNthItem(0);
    //getChild<LLComboBox>("local_size_combo")->selectNthItem(0);
    //getChild<LLComboBox>("local_format_combo")->selectNthItem(0);
    // </FS:Ansariel>
    mOriginalHeight = getRect().getHeight();
    impl->mPreviewHandle = previewp->getHandle();
    previewp->setContainer(this);
    impl->updateControls(this);
    impl->setAdvanced(gSavedSettings.getBOOL("AdvanceSnapshot"));
    impl->updateLayout(this);


    previewp->setThumbnailPlaceholderRect(getThumbnailPlaceholderRect());

    return true;
}

// virtual
void LLFloaterSnapshotBase::draw()
{
    LLSnapshotLivePreview* previewp = getPreviewView();

    if (previewp && (previewp->isSnapshotActive() || previewp->getThumbnailLock()))
    {
        // don't render snapshot window in snapshot, even if "show ui" is turned on
        return;
    }

    LLFloater::draw();

    if (previewp && !isMinimized() && mThumbnailPlaceholder->getVisible())
    {
        if(previewp->getThumbnailImage())
        {
            bool working = impl->getStatus() == ImplBase::STATUS_WORKING;
            const LLRect& thumbnail_rect = getThumbnailPlaceholderRect();
            const S32 thumbnail_w = previewp->getThumbnailWidth();
            const S32 thumbnail_h = previewp->getThumbnailHeight();

            // calc preview offset within the preview rect
            const S32 local_offset_x = (thumbnail_rect.getWidth() - thumbnail_w) / 2 ;
            const S32 local_offset_y = (thumbnail_rect.getHeight() - thumbnail_h) / 2 ; // preview y pos within the preview rect

            // calc preview offset within the floater rect
            S32 offset_x = thumbnail_rect.mLeft + local_offset_x;
            S32 offset_y = thumbnail_rect.mBottom + local_offset_y;

            gGL.matrixMode(LLRender::MM_MODELVIEW);
            // Apply floater transparency to the texture unless the floater is focused.
            F32 alpha = getTransparencyType() == TT_ACTIVE ? 1.0f : getCurrentTransparency();
            LLColor4 color = working ? LLColor4::grey4 : LLColor4::white;
            gl_draw_scaled_image(offset_x, offset_y,
                    thumbnail_w, thumbnail_h,
                    previewp->getThumbnailImage(), color % alpha);

            previewp->drawPreviewRect(offset_x, offset_y) ;

            gGL.pushUIMatrix();
            LLUI::translate((F32) thumbnail_rect.mLeft, (F32) thumbnail_rect.mBottom);
            mThumbnailPlaceholder->draw();
            gGL.popUIMatrix();
        }
    }
    impl->updateLayout(this);
}

//virtual
void LLFloaterSnapshot::onOpen(const LLSD& key)
{
    LLSnapshotLivePreview* preview = getPreviewView();
    if(preview)
    {
        LL_DEBUGS() << "opened, updating snapshot" << LL_ENDL;
        preview->setAllowFullScreenPreview(true);
        preview->updateSnapshot(true);
    }
    focusFirstItem(false);
    gSnapshotFloaterView->setEnabled(true);
    gSnapshotFloaterView->setVisible(true);
    gSnapshotFloaterView->adjustToFitScreen(this, false);

    impl->updateControls(this);
    impl->setAdvanced(gSavedSettings.getBOOL("AdvanceSnapshot"));
    impl->updateLayout(this);

    // <FS:Ansariel> FIRE-16145: CTRL-SHIFT-S doesn't update the snapshot anymore
    if (mIsOpen)
    {
        return;
    }
    mIsOpen = true;
    // </FS:Ansariel>

    // Initialize default tab.
    // <FS:Ansariel> Don't return to target selection after taking a snapshot
    //getChild<LLSideTrayPanelContainer>("panel_container")->getCurrentPanel()->onOpen(LLSD());
    LLSideTrayPanelContainer* panel_container = getChild<LLSideTrayPanelContainer>("panel_container");
    std::string last_snapshot_panel = gSavedSettings.getString("FSLastSnapshotPanel");
    panel_container->selectTabByName(last_snapshot_panel.empty() ? "panel_snapshot_options" : last_snapshot_panel);
    panel_container->getCurrentPanel()->onOpen(LLSD());
    mSucceessLblPanel->setVisible(false);
    mFailureLblPanel->setVisible(false);
    // </FS:Ansariel>

// <FS:CR> FIRE-9621
#ifdef OPENSIM
    if (!LLGridManager::getInstance()->isInSecondLife())
    {
        LLLayoutStack* stackcontainer = findChild<LLLayoutStack>("option_buttons");
        if (stackcontainer)
        {
            LLLayoutPanel* panel_snapshot_profile = stackcontainer->findChild<LLLayoutPanel>("lp_profile");
            if (panel_snapshot_profile)
            {
                panel_snapshot_profile->setVisible(false);
            }
            LLLayoutPanel* panel_snapshot_facebook = stackcontainer->findChild<LLLayoutPanel>("lp_facebook");
            if (panel_snapshot_facebook)
            {
                panel_snapshot_facebook->setVisible(false);
            }
            LLLayoutPanel* panel_snapshot_twitter = stackcontainer->findChild<LLLayoutPanel>("lp_twitter");
            if (panel_snapshot_twitter)
            {
                panel_snapshot_twitter->setVisible(false);
            }
        }
    }
#endif // OPENSIM
// </FS:CR>
}

void LLFloaterSnapshot::onExtendFloater()
{
    impl->setAdvanced(gSavedSettings.getBOOL("AdvanceSnapshot"));
}

void LLFloaterSnapshot::on360Snapshot()
{
    LLFloaterReg::showInstance("360capture");
    closeFloater();
}

// <FS:Ansariel> FIRE-16043: Remember last used snapshot option
//virtual
void LLFloaterSnapshot::onClose(bool app_quitting)
{
    LLFloaterSnapshotBase::onClose(app_quitting);

    // <FS:Ansariel> FIRE-16145: CTRL-SHIFT-S doesn't update the snapshot anymore
    mIsOpen = false;

    LLSideTrayPanelContainer* panel_container = getChild<LLSideTrayPanelContainer>("panel_container");
    gSavedSettings.setString("FSLastSnapshotPanel", panel_container->getCurrentPanel()->getName());
}
// </FS:Ansariel>

//virtual
void LLFloaterSnapshotBase::onClose(bool app_quitting)
{
    getParent()->setMouseOpaque(false);

    //unfreeze everything, hide fullscreen preview
    LLSnapshotLivePreview* previewp = getPreviewView();
    if (previewp)
    {
        previewp->setAllowFullScreenPreview(false);
        previewp->setVisible(false);
        previewp->setEnabled(false);
    }

    gSavedSettings.setBOOL("FreezeTime", false);
    impl->mAvatarPauseHandles.clear();

    if (impl->mLastToolset)
    {
        LLToolMgr::getInstance()->setCurrentToolset(impl->mLastToolset);
    }
}

// virtual
S32 LLFloaterSnapshotBase::notify(const LLSD& info)
{
    if (info.has("set-ready"))
    {
        impl->setStatus(ImplBase::STATUS_READY);
        return 1;
    }

    if (info.has("set-working"))
    {
        impl->setStatus(ImplBase::STATUS_WORKING);
        return 1;
    }

    if (info.has("set-finished"))
    {
        LLSD data = info["set-finished"];
        impl->setStatus(ImplBase::STATUS_FINISHED, data["ok"].asBoolean(), data["msg"].asString());
        return 1;
    }

    if (info.has("snapshot-updating"))
    {
        // Disable the send/post/save buttons until snapshot is ready.
        impl->updateControls(this);
        return 1;
    }

    if (info.has("snapshot-updated"))
    {
        // Enable the send/post/save buttons.
        impl->updateControls(this);
        // We've just done refresh.
        impl->setNeedRefresh(false);

        // The refresh button is initially hidden. We show it after the first update,
        // i.e. when preview appears.
        if (mRefreshBtn && !mRefreshBtn->getVisible())
        {
            mRefreshBtn->setVisible(true);
        }
        return 1;
    }

    return 0;
}

// virtual
S32 LLFloaterSnapshot::notify(const LLSD& info)
{
    bool res = LLFloaterSnapshotBase::notify(info);
    if (res)
        return res;
    // A child panel wants to change snapshot resolution.
    if (info.has("combo-res-change"))
    {
        std::string combo_name = info["combo-res-change"]["control-name"].asString();
        ((Impl*)impl)->updateResolution(getChild<LLUICtrl>(combo_name), this);
        return 1;
    }

    if (info.has("custom-res-change"))
    {
        LLSD res = info["custom-res-change"];
        ((Impl*)impl)->applyCustomResolution(this, res["w"].asInteger(), res["h"].asInteger());
        return 1;
    }

    if (info.has("keep-aspect-change"))
    {
        ((Impl*)impl)->applyKeepAspectCheck(this, info["keep-aspect-change"].asBoolean());
        return 1;
    }

    if (info.has("image-quality-change"))
    {
        ((Impl*)impl)->onImageQualityChange(this, info["image-quality-change"].asInteger());
        return 1;
    }

    if (info.has("image-format-change"))
    {
        ((Impl*)impl)->onImageFormatChange(this);
        return 1;
    }

    return 0;
}

bool LLFloaterSnapshot::isWaitingState()
{
    return (impl->getStatus() == ImplBase::STATUS_WORKING);
}

// <FS:Beq> FIRE-35002 - Post to flickr broken, improved solution
// bool LLFloaterSnapshotBase::ImplBase::updatePreviewList(bool initialized)
bool LLFloaterSnapshotBase::ImplBase::updatePreviewList(bool initialized, bool have_socials)
// </FS:Beq>
{
    // <FS:Ansariel> Share to Flickr
    //if (!initialized)
    if (!initialized && !have_socials)
    // </FS:Ansariel>
        return false;

    bool changed = false;
    LL_DEBUGS() << "npreviews: " << LLSnapshotLivePreview::sList.size() << LL_ENDL;
    for (std::set<LLSnapshotLivePreview*>::iterator iter = LLSnapshotLivePreview::sList.begin();
        iter != LLSnapshotLivePreview::sList.end(); ++iter)
    {
        changed |= LLSnapshotLivePreview::onIdle(*iter);
    }
    return changed;
}


void LLFloaterSnapshotBase::ImplBase::updateLivePreview()
{
    // don't update preview for hidden floater
    // <FS:Beq> FIRE-35002 - Post to flickr broken
    bool have_socials = (
        LLFloaterReg::findTypedInstance<LLFloaterFlickr>("flickr") != nullptr ||
        LLFloaterReg::findTypedInstance<FSFloaterPrimfeed>("primfeed") != nullptr
        );
    if ( ((mFloater && mFloater->isInVisibleChain()) ||
        have_socials) &&
        ImplBase::updatePreviewList(true, have_socials))
    // </FS:Beq>
    {
        LL_DEBUGS() << "changed" << LL_ENDL;
        updateControls(mFloater);
    }    
}

//static
void LLFloaterSnapshot::update()
{
    LLFloaterSnapshot* inst = findInstance();
    if (inst != NULL)
    {
        inst->impl->updateLivePreview();
    }
    else
    {
        ImplBase::updatePreviewList(false);
    }
}

// static
LLFloaterSnapshot* LLFloaterSnapshot::findInstance()
{
    return LLFloaterReg::findTypedInstance<LLFloaterSnapshot>("snapshot");
}

// static
LLFloaterSnapshot* LLFloaterSnapshot::getInstance()
{
    return LLFloaterReg::getTypedInstance<LLFloaterSnapshot>("snapshot");
}

// virtual
void LLFloaterSnapshot::saveTexture()
{
    LL_DEBUGS() << "saveTexture" << LL_ENDL;

    LLSnapshotLivePreview* previewp = getPreviewView();
    if (!previewp)
    {
        llassert(previewp != NULL);
        return;
    }

    previewp->saveTexture();
}

void LLFloaterSnapshot::saveLocal(const snapshot_saved_signal_t::slot_type& success_cb, const snapshot_saved_signal_t::slot_type& failure_cb)
{
    LL_DEBUGS() << "saveLocal" << LL_ENDL;
    LLSnapshotLivePreview* previewp = getPreviewView();
    llassert(previewp != NULL);
    if (previewp)
    {
        previewp->saveLocal(success_cb, failure_cb);
    }
}

void LLFloaterSnapshotBase::postSave()
{
    impl->updateControls(this);
    impl->setStatus(ImplBase::STATUS_WORKING);
}

// virtual
void LLFloaterSnapshotBase::postPanelSwitch()
{
    impl->updateControls(this);

    // Remove the success/failure indicator whenever user presses a snapshot option button.
    impl->setStatus(ImplBase::STATUS_READY);

    // <FS:Ansariel> Enable spinners and aspect ratio checkbox only for custom resolution
    LLPanelSnapshot* panel = impl->getActivePanel(this);
    if (panel)
    {
        std::string sdstring = panel->getImageSizeComboBox()->getSelectedValue();
        LLSD sdres;
        std::stringstream sstream(sdstring);
        LLSDSerialize::fromNotation(sdres, sstream, sdstring.size());
        bool is_custom_resolution = (sdres[0].asInteger() == -1 && sdres[1].asInteger() == -1);

        panel->enableAspectRatioCheckbox(is_custom_resolution);
        panel->getWidthSpinner()->setEnabled(is_custom_resolution);
        panel->getHeightSpinner()->setEnabled(is_custom_resolution);
    }
    // </FS:Ansariel>
}

void LLFloaterSnapshotBase::inventorySaveFailed()
{
    impl->updateControls(this);
    impl->setStatus(ImplBase::STATUS_FINISHED, false, "inventory");
}

// static
LLPointer<LLImageFormatted> LLFloaterSnapshotBase::getImageData()
{
    // FIXME: May not work for textures.

    LLSnapshotLivePreview* previewp = getPreviewView();
    if (!previewp)
    {
        llassert(previewp != NULL);
        return NULL;
    }

    LLPointer<LLImageFormatted> img = previewp->getFormattedImage();
    if (!img.get())
    {
        LL_WARNS() << "Empty snapshot image data" << LL_ENDL;
        llassert(img.get() != NULL);
    }

    return img;
}

const LLVector3d& LLFloaterSnapshotBase::getPosTakenGlobal()
{
    LLSnapshotLivePreview* previewp = getPreviewView();
    if (!previewp)
    {
        llassert(previewp != NULL);
        return LLVector3d::zero;
    }

    return previewp->getPosTakenGlobal();
}

// static
void LLFloaterSnapshot::setAgentEmail(const std::string& email)
{
    LLFloaterSnapshot* instance = findInstance();
    if (instance)
    {
        LLSideTrayPanelContainer* panel_container = instance->getChild<LLSideTrayPanelContainer>("panel_container");
        LLPanel* postcard_panel = panel_container->getPanelByName("panel_snapshot_postcard");
        postcard_panel->notify(LLSD().with("agent-email", email));
    }
}

///----------------------------------------------------------------------------
/// Class LLSnapshotFloaterView
///----------------------------------------------------------------------------

LLSnapshotFloaterView::LLSnapshotFloaterView (const Params& p) : LLFloaterView (p)
{
}

LLSnapshotFloaterView::~LLSnapshotFloaterView()
{
}

// virtual
bool LLSnapshotFloaterView::handleKey(KEY key, MASK mask, bool called_from_parent)
{
    // use default handler when not in freeze-frame mode
    if(!gSavedSettings.getBOOL("FreezeTime"))
    {
        return LLFloaterView::handleKey(key, mask, called_from_parent);
    }

    if (called_from_parent)
    {
        // pass all keystrokes down
        LLFloaterView::handleKey(key, mask, called_from_parent);
    }
    else
    {
        // bounce keystrokes back down
        LLFloaterView::handleKey(key, mask, true);
    }
    return true;
}

// virtual
bool LLSnapshotFloaterView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    // use default handler when not in freeze-frame mode
    if(!gSavedSettings.getBOOL("FreezeTime"))
    {
        return LLFloaterView::handleMouseDown(x, y, mask);
    }
    // give floater a change to handle mouse, else camera tool
    if (childrenHandleMouseDown(x, y, mask) == NULL)
    {
        LLToolMgr::getInstance()->getCurrentTool()->handleMouseDown( x, y, mask );
    }
    return true;
}

// virtual
bool LLSnapshotFloaterView::handleMouseUp(S32 x, S32 y, MASK mask)
{
    // use default handler when not in freeze-frame mode
    if(!gSavedSettings.getBOOL("FreezeTime"))
    {
        return LLFloaterView::handleMouseUp(x, y, mask);
    }
    // give floater a change to handle mouse, else camera tool
    if (childrenHandleMouseUp(x, y, mask) == NULL)
    {
        LLToolMgr::getInstance()->getCurrentTool()->handleMouseUp( x, y, mask );
    }
    return true;
}

// virtual
bool LLSnapshotFloaterView::handleHover(S32 x, S32 y, MASK mask)
{
    // use default handler when not in freeze-frame mode
    if(!gSavedSettings.getBOOL("FreezeTime"))
    {
        return LLFloaterView::handleHover(x, y, mask);
    }
    // give floater a change to handle mouse, else camera tool
    if (childrenHandleHover(x, y, mask) == NULL)
    {
        LLToolMgr::getInstance()->getCurrentTool()->handleHover( x, y, mask );
    }
    return true;
}

///----------------------------------------------------------------------------
/// Class LLFloaterSnapshotAvatarSelect
///----------------------------------------------------------------------------

LLFloaterSnapshotAvatarSelect::LLFloaterSnapshotAvatarSelect(const LLSD& key)
:   LLFloater(key),
    mAvatarList(nullptr),
    mFilterEditor(nullptr),
    mFilterString()
{
}

LLFloaterSnapshotAvatarSelect::~LLFloaterSnapshotAvatarSelect()
{
}

// static
void LLFloaterSnapshotAvatarSelect::onAvatarNameCacheCallback(LLHandle<LLFloaterSnapshotAvatarSelect> handle, const LLUUID& agent_id, const LLAvatarName& av_name)
{
    LLFloaterSnapshotAvatarSelect* floater = handle.get();
    if (floater)
    {
        floater->onAvatarNameCache(agent_id, av_name);
    }
}

// static
void LLFloaterSnapshotAvatarSelect::onCheckItemCallback(LLHandle<LLFloaterSnapshotAvatarSelect> handle, LLUICtrl* ctrl, const LLUUID& id)
{
    LLFloaterSnapshotAvatarSelect* floater = handle.get();
    if (floater)
    {
        floater->onCheckItem(ctrl, id);
    }
}

bool LLFloaterSnapshotAvatarSelect::postBuild()
{
    mAvatarList = getChild<LLScrollListCtrl>("avatar_list");
    mFilterEditor = getChild<LLFilterEditor>("avatar_filter");

    if (mFilterEditor)
    {
        mFilterEditor->setCommitCallback(boost::bind(&LLFloaterSnapshotAvatarSelect::onFilterEdit, this, _2));
    }

    childSetAction("select_all_btn", boost::bind(&LLFloaterSnapshotAvatarSelect::onSelectAll, this));
    childSetAction("clear_btn", boost::bind(&LLFloaterSnapshotAvatarSelect::onDeselectAll, this));
    childSetAction("refresh_btn", boost::bind(&LLFloaterSnapshotAvatarSelect::refreshList, this));

    return LLFloater::postBuild();
}

void LLFloaterSnapshotAvatarSelect::onOpen(const LLSD& key)
{
    refreshList();
}

void LLFloaterSnapshotAvatarSelect::refreshList()
{
    mNearbyAvatars.clear();
    LLVector3d camera_pos = gAgentCamera.getCameraPositionGlobal();

    std::set<LLUUID> seen_ids;
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = (LLVOAvatar*)character;
        if (avatar && !avatar->isDead() && !avatar->mIsDummy && !avatar->isOrphaned() && !avatar->isControlAvatar() && !avatar->isSelf())
        {
            LLUUID av_id = avatar->getID();
            if (seen_ids.insert(av_id).second)
            {
                NearbyAvatarInfo info;
                info.id = av_id;
                info.distance = (F32)(avatar->getPositionGlobal() - camera_pos).magVec();

                LLAvatarName av_name;
                if (LLAvatarNameCache::get(av_id, &av_name))
                {
                    info.displayName = av_name.getDisplayName();
                    info.userName = av_name.getUserName();
                }
                else
                {
                    info.displayName = gCacheName ? gCacheName->getDefaultName() : "Loading...";
                    info.userName = "";
                    LLAvatarNameCache::get(av_id, boost::bind(&LLFloaterSnapshotAvatarSelect::onAvatarNameCacheCallback, getDerivedHandle<LLFloaterSnapshotAvatarSelect>(), _1, _2));
                }
                mNearbyAvatars.push_back(info);
            }
        }
    }

    // Sort by distance (closest to camera first)
    std::sort(mNearbyAvatars.begin(), mNearbyAvatars.end(), [](const NearbyAvatarInfo& a, const NearbyAvatarInfo& b) {
        return a.distance < b.distance;
    });

    if (!mAvatarList) return;

    mAvatarList->deleteAllItems();

    std::string filter_lower = mFilterString;
    LLStringUtil::toLower(filter_lower);

    for (const auto& info : mNearbyAvatars)
    {
        if (!filter_lower.empty())
        {
            std::string disp_lower = info.displayName;
            std::string user_lower = info.userName;
            LLStringUtil::toLower(disp_lower);
            LLStringUtil::toLower(user_lower);
            if (disp_lower.find(filter_lower) == std::string::npos &&
                user_lower.find(filter_lower) == std::string::npos)
            {
                continue;
            }
        }

        LLSD row;
        row["id"] = info.id;

        bool is_checked = (gSnapshotSelectedAvatars.count(info.id) > 0);

        LLSD& columns = row["columns"];
        columns[0]["column"] = "check";
        columns[0]["type"] = "checkbox";
        columns[0]["value"] = is_checked;

        std::string full_name = info.displayName;
        if (!info.userName.empty() && info.userName != info.displayName)
        {
            full_name += " (" + info.userName + ")";
        }
        columns[1]["column"] = "name";
        columns[1]["type"] = "text";
        columns[1]["value"] = full_name;

        columns[2]["column"] = "distance";
        columns[2]["type"] = "text";
        columns[2]["value"] = llformat("%.1fm", info.distance);

        LLScrollListItem* item = mAvatarList->addElement(row);
        if (item)
        {
            LLScrollListCheck* check_cell = dynamic_cast<LLScrollListCheck*>(item->getColumn(0));
            if (check_cell && check_cell->getCheckBox())
            {
                check_cell->getCheckBox()->setCommitCallback(boost::bind(&LLFloaterSnapshotAvatarSelect::onCheckItemCallback, getDerivedHandle<LLFloaterSnapshotAvatarSelect>(), _1, info.id));
            }
        }
    }
}

void LLFloaterSnapshotAvatarSelect::onCheckItem(LLUICtrl* ctrl, const LLUUID& id)
{
    LLCheckBoxCtrl* checkbox = dynamic_cast<LLCheckBoxCtrl*>(ctrl);
    if (checkbox)
    {
        if (checkbox->get())
        {
            gSnapshotSelectedAvatars.insert(id);
        }
        else
        {
            gSnapshotSelectedAvatars.erase(id);
        }
        updatePreview();
    }
}

void LLFloaterSnapshotAvatarSelect::onSelectAll()
{
    for (const auto& info : mNearbyAvatars)
    {
        gSnapshotSelectedAvatars.insert(info.id);
    }
    refreshList();
    updatePreview();
}

void LLFloaterSnapshotAvatarSelect::onDeselectAll()
{
    gSnapshotSelectedAvatars.clear();
    refreshList();
    updatePreview();
}

void LLFloaterSnapshotAvatarSelect::onFilterEdit(const LLSD& value)
{
    mFilterString = value.asString();
    refreshList();
}

void LLFloaterSnapshotAvatarSelect::onAvatarNameCache(const LLUUID& agent_id, const LLAvatarName& av_name)
{
    for (auto& info : mNearbyAvatars)
    {
        if (info.id == agent_id)
        {
            info.displayName = av_name.getDisplayName();
            info.userName = av_name.getUserName();
            break;
        }
    }
    if (mAvatarList)
    {
        LLScrollListItem* item = mAvatarList->getItem(agent_id);
        if (item)
        {
            std::string full_name = av_name.getDisplayName();
            if (!av_name.getUserName().empty() && av_name.getUserName() != av_name.getDisplayName())
            {
                full_name += " (" + av_name.getUserName() + ")";
            }
            LLScrollListCell* name_cell = item->getColumn(1);
            if (name_cell)
            {
                name_cell->setValue(full_name);
            }
        }
    }
}

void LLFloaterSnapshotAvatarSelect::updatePreview()
{
    LLFloaterSnapshot* snap_floater = LLFloaterReg::findTypedInstance<LLFloaterSnapshot>("snapshot");
    if (snap_floater)
    {
        LLSnapshotLivePreview* previewp = snap_floater->getPreviewView();
        if (previewp)
        {
            previewp->updateSnapshot(true, true);
        }
    }
}

void LLFloaterSnapshotAvatarSelect::positionBeside(LLFloater* parent_floater)
{
    if (!parent_floater) return;

    LLRect parent_rect = parent_floater->getRect();
    LLRect my_rect = getRect();
    S32 my_w = my_rect.getWidth();
    S32 my_h = my_rect.getHeight();

    S32 screen_w = gViewerWindow ? gViewerWindow->getWindowWidthRaw() : 1024;

    S32 new_left = parent_rect.mRight + 2;
    S32 new_top = parent_rect.mTop;

    if (new_left + my_w > screen_w)
    {
        new_left = parent_rect.mLeft - my_w - 2;
    }
    if (new_left < 0)
    {
        new_left = 10;
    }

    setRect(LLRect(new_left, new_top, new_left + my_w, new_top - my_h));
}
