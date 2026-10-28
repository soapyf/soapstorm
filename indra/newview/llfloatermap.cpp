/**
 * @file llfloatermap.cpp
 * @brief The "mini-map" or radar in the upper right part of the screen.
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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

// self include
#include "llfloatermap.h"

// Library includes
#include "llfloaterreg.h"
#include "llfontgl.h"
#include "llglheaders.h"

// Viewer includes
#include "llagentcamera.h"
#include "llviewercontrol.h"
#include "llnetmap.h"
#include "lltracker.h"
#include "llviewercamera.h"
#include "lldraghandle.h"
#include "lltextbox.h"
#include "llfloaterworldmap.h"
#include "llagent.h"
#include "gmdxminimapcompass.h" // <GMDX>
#include "llfocusmgr.h" // <GMDX>

//
// Constants
//

// The minor cardinal direction labels are hidden if their height is more
// than this proportion of the map.
// <FS:Ansariel> FIRE-32341: Make minor directions visible at reasonable mini map size again
//const F32 MAP_MINOR_DIR_THRESHOLD = 0.035f;
const F32 MAP_MINOR_DIR_THRESHOLD = 0.07f;

const S32 MAP_PADDING_LEFT = 0;
const S32 MAP_PADDING_TOP = 2;
const S32 MAP_PADDING_RIGHT = 2;
const S32 MAP_PADDING_BOTTOM = 0;
//
// Member functions
//

LLFloaterMap::LLFloaterMap(const LLSD& key)
    : LLFloater(key),
      mTextBoxEast(NULL),
      mTextBoxNorth(NULL),
      mTextBoxWest(NULL),
      mTextBoxSouth(NULL),
      mTextBoxSouthEast(NULL),
      mTextBoxNorthEast(NULL),
      mTextBoxNorthWest(NULL),
      mTextBoxSouthWest(NULL),
      mMap(NULL)
{
}

LLFloaterMap::~LLFloaterMap()
{
}

bool LLFloaterMap::postBuild()
{
    mMap = getChild<LLNetMap>("Net Map");
    mMap->setToolTipMsg(getString("ToolTipMsg"));
    mMap->setParcelNameMsg(getString("ParcelNameMsg"));
    mMap->setParcelSalePriceMsg(getString("ParcelSalePriceMsg"));
    mMap->setParcelSaleAreaMsg(getString("ParcelSaleAreaMsg"));
    mMap->setParcelOwnerMsg(getString("ParcelOwnerMsg"));
    mMap->setRegionNameMsg(getString("RegionNameMsg"));
    mMap->setToolTipHintMsg(getString("ToolTipHintMsg"));
    mMap->setAltToolTipHintMsg(getString("AltToolTipHintMsg"));
    sendChildToBack(mMap);

    mTextBoxNorth     = getChild<LLTextBox>("floater_map_north");
    mTextBoxEast      = getChild<LLTextBox>("floater_map_east");
    mTextBoxWest      = getChild<LLTextBox>("floater_map_west");
    mTextBoxSouth     = getChild<LLTextBox>("floater_map_south");
    mTextBoxSouthEast = getChild<LLTextBox>("floater_map_southeast");
    mTextBoxNorthEast = getChild<LLTextBox>("floater_map_northeast");
    mTextBoxSouthWest = getChild<LLTextBox>("floater_map_southwest");
    mTextBoxNorthWest = getChild<LLTextBox>("floater_map_northwest");

    mTextBoxNorth->reshapeToFitText();
    mTextBoxEast->reshapeToFitText();
    mTextBoxWest->reshapeToFitText();
    mTextBoxSouth->reshapeToFitText();
    mTextBoxSouthEast->reshapeToFitText();
    mTextBoxNorthEast->reshapeToFitText();
    mTextBoxSouthWest->reshapeToFitText();
    mTextBoxNorthWest->reshapeToFitText();


    // <FS:Ansariel> Remove titlebar
    stretchMiniMap(getRect().getWidth() - MAP_PADDING_LEFT - MAP_PADDING_RIGHT,
        getRect().getHeight() - MAP_PADDING_TOP - MAP_PADDING_BOTTOM);

    updateMinorDirections();

    // Get the drag handle all the way in back
    sendChildToBack(getDragHandle());

    // <FS:Ansariel> Remove titlebar
    setIsChrome(true);
    getDragHandle()->setTitleVisible(true);
    // </FS:Ansariel>

    // keep onscreen
    gFloaterView->adjustToFitScreen(this, false);

    // <GMDX> Show Whole Region keeps the mini-map square: squared when it is turned on, and when
    // the compass style or the rotation changes (the heading tape adds its strip above the map)
    mWholeRegionConn = gSavedSettings.getControl("GMDXMiniMapWholeRegion")->getSignal()->connect(
        [this](LLControlVariable*, const LLSD& value, const LLSD&)
        {
            if (value.asBoolean())
            {
                makeSquare();
            }
        });
    auto resquare = [this](LLControlVariable*, const LLSD&, const LLSD&)
    {
        if (gSavedSettings.getBOOL("GMDXMiniMapWholeRegion"))
        {
            makeSquare();
        }
    };
    mCompassStyleConn = gSavedSettings.getControl("GMDXMiniMapCompassStyle")->getSignal()->connect(resquare);
    mRotateConn = gSavedSettings.getControl("MiniMapRotate")->getSignal()->connect(resquare);   // (the tape is north-up only)
    // </GMDX>

    return true;
}

bool LLFloaterMap::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    // If floater is minimized, minimap should be shown on doubleclick (STORM-299)
    if (isMinimized())
    {
        setMinimized(false);
        return true;
    }

    LLVector3d pos_global = mMap->viewPosToGlobal(x, y);

    // <FS:Ansariel> Synchronize double click handling throughout instances
    //LLTracker::stopTracking(false);
    //LLFloaterWorldMap* world_map = LLFloaterWorldMap::getInstance();
    //if (world_map)
    //{
    //  world_map->trackLocation(pos_global);
    //}
    //
    //if (gSavedSettings.getBOOL("DoubleClickTeleport"))
    //{
    //  // If DoubleClickTeleport is on, double clicking the minimap will teleport there
    //  gAgent.teleportViaLocationLookAt(pos_global);
    //}
    //else if (gSavedSettings.getBOOL("DoubleClickShowWorldMap"))
    //{
    //  LLFloaterReg::showInstance("world_map");
    //}
    mMap->performDoubleClickAction(pos_global);
    // </FS:Ansariel> Synchronize double click handling throughout instances
    return true;
}

void LLFloaterMap::setDirectionPos(LLTextBox *text_box, F32 rotation)
{
    // Rotation is in radians.
    // Rotation of 0 means x = 1, y = 0 on the unit circle.

    F32 map_half_height  = (F32) (getRect().getHeight() / 2) - (getHeaderHeight() / 2);
    F32 map_half_width   = (F32) (getRect().getWidth() / 2);
    F32 text_half_height = (F32) (text_box->getRect().getHeight() / 2);
    F32 text_half_width  = (F32) (text_box->getRect().getWidth() / 2);
    F32 extra_padding    = (F32) (mTextBoxNorth->getRect().getWidth() / 2);
    F32 pos_half_height  = map_half_height - text_half_height - extra_padding;
    F32 pos_half_width   = map_half_width - text_half_width - extra_padding;

    F32 corner_angle               = atan2(pos_half_height, pos_half_width);
    F32 rotation_mirrored_into_top = abs(fmodf(rotation, F_PI));
    if (rotation < 0)
    {
        rotation_mirrored_into_top = F_PI - rotation_mirrored_into_top;
    }
    F32  rotation_mirrored_into_top_right = (F_PI_BY_TWO - abs(rotation_mirrored_into_top - F_PI_BY_TWO));
    bool at_left_right_edge               = rotation_mirrored_into_top_right < corner_angle;

    F32 part_x = cos(rotation);
    F32 part_y = sin(rotation);
    F32 y;
    F32 x;
    if (at_left_right_edge)
    {
        x = std::copysign(pos_half_width, part_x);
        y = x * part_y / part_x;
    }
    else
    {
        y = std::copysign(pos_half_height, part_y);
        x = y * part_x / part_y;
    }

    text_box->setOrigin(ll_round(map_half_width + x - text_half_width), ll_round(map_half_height + y - text_half_height));
}

void LLFloaterMap::updateMinorDirections()
{
    if (mTextBoxNorthEast == NULL || mDirectionsHidden) // <GMDX> hidden in Show Whole Region
    {
        return;
    }

    // Hide minor directions if they cover too much of the map
    bool show_minors = mTextBoxNorthEast->getRect().getHeight() < MAP_MINOR_DIR_THRESHOLD *
        llmin(getRect().getWidth(), getRect().getHeight());

    mTextBoxNorthEast->setVisible(show_minors);
    mTextBoxNorthWest->setVisible(show_minors);
    mTextBoxSouthWest->setVisible(show_minors);
    mTextBoxSouthEast->setVisible(show_minors);
}

// <GMDX> The floater-edge direction letters; hidden in Show Whole Region, whose compass
// (gmdxminimapcompass.h) draws its own.
void LLFloaterMap::setDirectionsVisible(bool visible)
{
    mDirectionsHidden = !visible;
    mTextBoxNorth->setVisible(visible);
    mTextBoxEast->setVisible(visible);
    mTextBoxWest->setVisible(visible);
    mTextBoxSouth->setVisible(visible);
    if (visible)
    {
        updateMinorDirections();
    }
    else
    {
        mTextBoxNorthEast->setVisible(false);
        mTextBoxNorthWest->setVisible(false);
        mTextBoxSouthWest->setVisible(false);
        mTextBoxSouthEast->setVisible(false);
    }
}

// Squares the floater (Show Whole Region shows a square region, plus the heading tape's strip),
// keeping its top edge and the side nearer its parent's edge where they are.
void LLFloaterMap::makeSquare()
{
    if (isMinimized())
    {
        return;
    }
    LLRect rect = getRect();
    const S32 extra = (S32)GMDXMinimapCompass::getTopReserve() + MAP_PADDING_TOP + MAP_PADDING_BOTTOM - MAP_PADDING_LEFT - MAP_PADDING_RIGHT;
    const S32 side = llmax(llmin(rect.getWidth(), rect.getHeight() - extra), getMinWidth(), getMinHeight());
    const LLView* parent = getParent();
    const bool keep_right = parent && rect.getCenterX() > parent->getRect().getWidth() / 2;
    if (keep_right)
    {
        rect.mLeft = rect.mRight - side;
    }
    else
    {
        rect.mRight = rect.mLeft + side;
    }
    rect.mBottom = rect.mTop - side - extra;
    setShape(rect, true);
}

// While the user drags a resize handle the floater resizes freely (the map stays square, centred
// inside it), and when the drag ends it snaps to the map's size, anchored at the edges the drag
// did not move. (Squaring on every resize event fights the resize handle: it carries the
// difference into the next event, and the size flips between two values while dragging.)
bool LLFloaterMap::isUserResizing() const
{
    const LLView* capture = dynamic_cast<const LLView*>(gFocusMgr.getMouseCapture());
    return capture && capture->hasAncestor(this);
}

void LLFloaterMap::handleReshape(const LLRect& new_rect, bool by_user)
{
    static LLCachedControl<bool> whole_region(gSavedSettings, "GMDXMiniMapWholeRegion", false);
    if (by_user && whole_region && !isMinimized() && isUserResizing() && mDragStartRect.isEmpty())
    {
        mDragStartRect = getRect();   // a drag begins: remember where it started (see draw)
    }
    LLFloater::handleReshape(new_rect, by_user);
}

// After a resize drag in Show Whole Region: fit the floater to the map's square, keeping the edges
// the drag did not move.
void LLFloaterMap::fitAfterResize()
{
    const LLRect start = mDragStartRect;
    mDragStartRect = LLRect();
    if (isMinimized())
    {
        return;
    }
    LLRect rect = getRect();
    const S32 extra = (S32)GMDXMinimapCompass::getTopReserve() + MAP_PADDING_TOP + MAP_PADDING_BOTTOM - MAP_PADDING_LEFT - MAP_PADDING_RIGHT;
    const S32 side = llmax(llmin(rect.getWidth(), rect.getHeight() - extra), getMinWidth(), getMinHeight());
    if (rect.getWidth() == side && rect.getHeight() == side + extra)
    {
        return;
    }
    if (rect.mLeft != start.mLeft && rect.mRight == start.mRight)
    {
        rect.mLeft = rect.mRight - side;
    }
    else
    {
        rect.mRight = rect.mLeft + side;
    }
    if (rect.mTop != start.mTop && rect.mBottom == start.mBottom)
    {
        rect.mTop = rect.mBottom + side + extra;
    }
    else
    {
        rect.mBottom = rect.mTop - side - extra;   // the top stays when neither or both moved
    }
    setShape(rect, false);
}
// </GMDX>

// virtual
void LLFloaterMap::draw()
{
    // <GMDX> Show Whole Region: a resize drag that just ended snaps the floater to the map, and
    // the map's compass replaces the direction letters
    if (!mDragStartRect.isEmpty() && !isUserResizing())
    {
        fitAfterResize();
    }
    if (mMap->isWholeRegion() != mDirectionsHidden)
    {
        setDirectionsVisible(!mMap->isWholeRegion());
    }
    if (!mDirectionsHidden) // (hidden letters are not moved)
    {
    // </GMDX>
    F32 rotation = 0;

    static LLUICachedControl<bool> rotate_map("MiniMapRotate", true);
    if( rotate_map )
    {
        // rotate subsequent draws to agent rotation
        rotation = atan2( LLViewerCamera::getInstance()->getAtAxis().mV[VX], LLViewerCamera::getInstance()->getAtAxis().mV[VY] );
    }

    setDirectionPos( mTextBoxEast,  rotation );
    setDirectionPos( mTextBoxNorth, rotation + F_PI_BY_TWO );
    setDirectionPos( mTextBoxWest,  rotation + F_PI );
    setDirectionPos( mTextBoxSouth, rotation + F_PI + F_PI_BY_TWO );

    setDirectionPos( mTextBoxNorthEast, rotation +                      F_PI_BY_TWO / 2);
    setDirectionPos( mTextBoxNorthWest, rotation + F_PI_BY_TWO +        F_PI_BY_TWO / 2);
    setDirectionPos( mTextBoxSouthWest, rotation + F_PI +               F_PI_BY_TWO / 2);
    setDirectionPos( mTextBoxSouthEast, rotation + F_PI + F_PI_BY_TWO + F_PI_BY_TWO / 2);
    } // <GMDX>

    // Note: we can't just gAgent.check cameraMouselook() because the transition states are wrong.
    if(gAgentCamera.cameraMouselook())
    {
        setMouseOpaque(false);
        getDragHandle()->setMouseOpaque(false);
    }
    else
    {
        setMouseOpaque(true);
        getDragHandle()->setMouseOpaque(true);
    }

    LLFloater::draw();
}

void LLFloaterMap::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLFloater::reshape(width, height, called_from_parent);

    // <FS:Ansariel> Remove titlebar
    stretchMiniMap(width - MAP_PADDING_LEFT - MAP_PADDING_RIGHT, height - MAP_PADDING_TOP - MAP_PADDING_BOTTOM);

    updateMinorDirections();
}

LLFloaterMap* LLFloaterMap::getInstance()
{
    return LLFloaterReg::getTypedInstance<LLFloaterMap>("mini_map");
}

// <FS:Ansariel> FIRE-1825: Minimap floater background transparency
// virtual
F32 LLFloaterMap::getCurrentTransparency()
{
    static LLCachedControl<F32> fsMiniMapOpacity(gSavedSettings, "FSMiniMapOpacity");
    return fsMiniMapOpacity();
}

// <FS:Ansariel> Remove titlebar
void LLFloaterMap::setMinimized(bool b)
{
    LLFloater::setMinimized(b);
    if (b)
    {
        setTitle(getString("mini_map_caption"));
    }
    else
    {
        setTitle("");
    }
}

void LLFloaterMap::stretchMiniMap(S32 width,S32 height)
{
    //fix for ext-7112
    //by default ctrl can't overlap caption area
    if(mMap)
    {
        LLRect map_rect;
        map_rect.setLeftTopAndSize( MAP_PADDING_LEFT, getRect().getHeight() - MAP_PADDING_TOP, width, height);
        mMap->reshape( width, height, 1);
        mMap->setRect(map_rect);
    }
}
// </FS:Ansariel>
