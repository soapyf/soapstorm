/**
 * @file llnetmap.h
 * @brief A little map of the world with network information
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

#ifndef LL_LLNETMAP_H
#define LL_LLNETMAP_H

#include "llmath.h"
#include "lluictrl.h"
#include "v3math.h"
#include "v3dmath.h"
#include "v4color.h"
#include "llpointer.h"
#include "llcoord.h"
#include "llrect.h" // <GMDX>

class LLColor4U;
class LLImageRaw;
class LLViewerTexture;
class LLFloaterMap;
class LLMenuGL;
// [SL:KB] - Patch: World-MinimapOverlay | Checked: 2012-06-20 (Catznip-3.3)
class LLViewerRegion;
class LLAvatarName;
// [/SL:KB]

class LLNetMap : public LLUICtrl
{
public:
    struct Params
    :   public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Optional<LLUIColor> bg_color;

        Params()
        :   bg_color("bg_color")
        {}
    };

protected:
    LLNetMap (const Params & p);
    friend class LLUICtrlFactory;
    friend class LLFloaterMap;

public:
    virtual ~LLNetMap();

    static const F32 MAP_SCALE_MIN;
    static const F32 MAP_SCALE_FAR;
    static const F32 MAP_SCALE_MEDIUM;
    static const F32 MAP_SCALE_CLOSE;
    static const F32 MAP_SCALE_VERY_CLOSE;
    static const F32 MAP_SCALE_MAX;

    /*virtual*/ void    draw();
    /*virtual*/ bool    handleScrollWheel(S32 x, S32 y, S32 clicks);
    /*virtual*/ bool    handleMouseDown(S32 x, S32 y, MASK mask);
    /*virtual*/ bool    handleMouseUp(S32 x, S32 y, MASK mask);
    /*virtual*/ bool    handleHover( S32 x, S32 y, MASK mask );
    void                onMouseLeave(S32 x, S32 y, MASK mask) override; // <GMDX> cursor coordinates
    /*virtual*/ bool    handleToolTip( S32 x, S32 y, MASK mask);
    /*virtual*/ void    reshape(S32 width, S32 height, bool called_from_parent = true);

    /*virtual*/ bool    postBuild();
    /*virtual*/ bool    handleRightMouseDown( S32 x, S32 y, MASK mask );
    /*virtual*/ bool    handleClick(S32 x, S32 y, MASK mask);
    /*virtual*/ bool    handleDoubleClick( S32 x, S32 y, MASK mask );

// [SL:KB] - Patch: World-MinimapOverlay | Checked: 2012-06-20 (Catznip-3.3)
    void            refreshParcelOverlay() { mUpdateParcelImage = true; }
// [/SL:KB]
    void            setScale(F32 scale);

    void            setToolTipMsg(const std::string& msg) { mToolTipMsg = msg; }
    void            setParcelNameMsg(const std::string& msg) { mParcelNameMsg = msg; }
    void            setParcelSalePriceMsg(const std::string& msg) { mParcelSalePriceMsg = msg; }
    void            setParcelSaleAreaMsg(const std::string& msg) { mParcelSaleAreaMsg = msg; }
    void            setParcelOwnerMsg(const std::string& msg) { mParcelOwnerMsg = msg; }
    void            setRegionNameMsg(const std::string& msg) { mRegionNameMsg = msg; }
    void            setToolTipHintMsg(const std::string& msg) { mToolTipHintMsg = msg; }
    void            setAltToolTipHintMsg(const std::string& msg) { mAltToolTipHintMsg = msg; }

    void            renderScaledPointGlobal( const LLVector3d& pos, const LLColor4U &color, F32 radius );
    LLVector3d      viewPosToGlobal(S32 x,S32 y);
    LLUUID          getClosestAgentToCursor() const { return mClosestAgentToCursor; }
    LLVector3d      getClosestAgentPosition() const { return mClosestAgentPosition; }

    // <FS:Ansariel> Synchronize double click handling throughout instances
    void            performDoubleClickAction(LLVector3d pos_global);

    // <GMDX> "Show Whole Region" (GMDXMiniMapWholeRegion) as last drawn: the agent's region
    // fitted to the view (LLFloaterMap keeps the floater square and hides its direction letters).
    bool            isWholeRegion() const { return mFitRegion; }
    // </GMDX>

    // <FS:Ansariel> Mark avatar feature
    static bool     hasAvatarMarkColor(const LLUUID& avatar_id) { return sAvatarMarksMap.find(avatar_id) != sAvatarMarksMap.end(); }
    static bool     getAvatarMarkColor(const LLUUID& avatar_id, LLColor4& color);
    static void     setAvatarMarkColor(const LLUUID& avatar_id, const LLSD& color);
    static void     setAvatarMarkColors(const uuid_vec_t& avatar_ids, const LLSD& color);
    static void     clearAvatarMarkColor(const LLUUID& avatar_id);
    static void     clearAvatarMarkColors(const uuid_vec_t& avatar_ids);
    static void     clearAvatarMarkColors();
    static LLColor4 getAvatarColor(const LLUUID& avatar_id);
    // </FS:Ansariel>

private:
    const LLVector3d& getObjectImageCenterGlobal()  { return mObjectImageCenterGlobal; }
    void            renderPoint(const LLVector3 &pos, const LLColor4U &color,
                                S32 diameter, S32 relative_height = 0);

    LLVector3       globalPosToView(const LLVector3d& global_pos);

    void            drawTracking( const LLVector3d& pos_global,
                                  const LLColor4& color,
                                  bool draw_arrow = true);
    void            drawRing(const F32 radius, LLVector3 pos_map, const LLUIColor& color);
    bool            isMouseOnPopupMenu();
    void            updateAboutLandPopupButton();
    bool            handleToolTipAgent(const LLUUID& avatar_id);
    static void     showAvatarInspector(const LLUUID& avatar_id);

// [SL:KB] - Patch: World-MinimapOverlay | Checked: 2012-06-20 (Catznip-3.3)
    bool            createImage(LLPointer<LLImageRaw>& rawimagep) const;
    void            createObjectImage();
    void            createParcelImage();

    F32             getScaleForName(std::string scale_name);
    void            renderPropertyLinesForRegion(const LLViewerRegion* pRegion, const LLColor4U& clrOverlay);
// [/SL:KB]
//  void            createObjectImage();

    static bool     outsideSlop(S32 x, S32 y, S32 start_x, S32 start_y, S32 slop);

    // <GMDX> (gmdxnetmap.cpp, but updateScaleDerived) Show Whole Region
    void            updateScaleDerived();   // pixels per meter, dot size, object layer texels per meter
    void            updateWholeRegion(LLViewerRegion* region, LLRectf& fit_square);   // per frame, before drawing
    void            drawRadarBackground();
    void            drawWholeRegionCompass(const LLRectf& fit_square);
    // A textured quad in map pixels, or its part on the round radar's disc (radius > 0) around the origin.
    static void     drawMapQuad(F32 x0, F32 y0, F32 x1, F32 y1, F32 u0, F32 v0, F32 u1, F32 v1, F32 radar_radius);
    void            addGMDXMenuCallbacks(LLUICtrl::CommitCallbackRegistry::ScopedRegistrar& commitRegistrar,
                                         LLUICtrl::EnableCallbackRegistry::ScopedRegistrar& enableRegistrar);
    LLVector3d      mapCenterGlobal() const;
    void            clampFitView(F32 region_width);
    LLVector2       mapPan() const { return mFitRegion ? mFitPan : mCurPan; }
    bool            fitPanAt(S32 x, S32 y) const;   // zoomed in, a plain drag here pans the region
    // Only Render Physical Objects
    void            drawPhysicalObjects(const LLVector3& map_center_agent, F32 pixels_per_meter, const LLViewerRegion* only_region);
    // Marks (gmdxminimapmarks.h) and the coordinates under the cursor
    void            drawMarks();
    void            drawCursorCoordinates();
    bool            onMap(const LLVector3& pos_local) const;   // inside the drawn map (Whole Region: its square / disc)
    LLUUID          markAt(S32 x, S32 y);                       // the mark drawn under (x, y), or null
    void            copyPopupPosition(bool slurl);
    void            addMarkAtPopup(S32 color);
    // Zones: Draw Zone puts the map in a drawing mode (mDrawKind: GMDXMapZone::Kind, -1 = off)
    void            drawZones();          // the saved zones: under the avatars
    void            drawZonePreview();    // the zone being drawn and its hint: over everything
    std::vector<LLVector2> zoneOutline(S32 kind, const std::vector<LLVector3d>& points);   // in view coordinates
    LLUUID          zoneAt(S32 x, S32 y);
    void            startZone(S32 kind);
    void            finishZone();
    // Right-click a dot > Dot Colour: "0".."5" (the mark colours), "custom" (the colour picker),
    // "clear"; for the closest avatar of the last right-click
    void            setAvatarDotColor(const std::string& which);
    void            destroyDotColorSwatch();
    void            updateGMDXPopupItems(LLMenuGL* menu, S32 x, S32 y);

    S32             mDrawKind = -1;
    std::vector<LLVector3d> mDrawPoints;
    LLVector3d      mDrawCursor;
    bool            mDrawDrag = false;
    LLUUID          mPopupZone;             // the zone the menu was opened on
    LLUUID          mPopupMark;             // the mark the menu was opened on
    bool            mCursorInside = false;
    S32             mCursorX = 0, mCursorY = 0;
    class LLColorSwatchCtrl* mDotColorSwatch = nullptr;   // hidden: the colour picker's owner
    LLUUID          mDotColorAvatar;        // whose colour the picker sets
    bool            mDotColorHad = false;   // its own colour before the picker opened, to restore on cancel
    LLColor4        mDotColorBefore;

    // Show Whole Region, as last drawn: the map centre (the region's centre, at the camera's height)
    // and scale replace the camera position and mScale.
    bool            mFitRegion = false;
    F32             mFitPixelsPerMeter = 0.f;
    LLVector3d      mFitCenterGlobal;
    LLVector2       mFitPan;                // the fitted square's offset from the view centre (heading tape room)
    // Zoom: 1 = the whole region; the view's centre in region metres (clamped so the view stays
    // inside the region); the region they belong to; the zoom-1 scale and the map's square in view
    // coordinates (for the wheel / drag handlers).
    F32             mFitZoom = 1.f;
    LLVector2       mFitViewCenter;
    U64             mFitRegionHandle = 0;
    F32             mFitBasePixelsPerMeter = 0.f;
    LLRectf         mFitMapRect;
    // Camera at top = the radar: centred on the agent, turned with the camera, cut to a disc of this
    // radius (view pixels) around the map centre; 0 = not the round radar.
    F32             mRadarRadius = 0.f;
    bool            mRadarSquare = false;   // the square one: turned inside mFitMapRect
    bool            radarOn() const { return mRadarRadius > 0.f || mRadarSquare; }
    LLVector2       radarCenter() const;    // the disc's centre in view coordinates
    LLVector2       toRadarRim(const LLVector2& p) const;   // p, pulled in to the rim when outside the disc / square
    // </GMDX>

//  bool            mUpdateNow;
// [SL:KB] - Patch: World-MinimapOverlay | Checked: 2012-06-20 (Catznip-3.3)
    bool            mUpdateObjectImage;
    bool            mUpdateParcelImage;
// [/SL:KB]

    LLUIColor       mBackgroundColor;

    F32             mScale;                 // Size of a region in pixels
    static F32      sScale;                 // <FS:Ansariel> Used to synchronize netmaps throughout instances

    F32             mPixelsPerMeter;        // world meters to map pixels
    F32             mObjectMapTPM;          // texels per meter on map
    F32             mObjectMapPixels;       // Width of object map in pixels
    F32             mDotRadius;             // Size of avatar markers

    bool            mPanning; // map is being dragged
    bool            mCentering; // map is being re-centered around the agent
    LLVector2       mCurPan;
    LLVector2       mStartPan; // pan offset at start of drag
    LLVector3d      mPopupWorldPos; // world position picked under mouse when context menu is opened
    LLCoordGL       mMouseDown; // pointer position at start of drag

    LLVector3d      mObjectImageCenterGlobal;
    LLPointer<LLImageRaw> mObjectRawImagep;
    LLPointer<LLViewerTexture>  mObjectImagep;
// [SL:KB] - Patch: World-MinimapOverlay | Checked: 2012-06-20 (Catznip-3.3)
    LLVector3d      mParcelImageCenterGlobal;
    LLPointer<LLImageRaw> mParcelRawImagep;
    LLPointer<LLViewerTexture>  mParcelImagep;

    boost::signals2::connection mParcelMgrConn;
    boost::signals2::connection mParcelOverlayConn;
// [/SL:KB]

    LLUUID          mClosestAgentToCursor;
// [SL:KB] - Patch: World-MiniMap | Checked: 2012-07-08 (Catznip-3.3)
    LLVector3d      mClosestAgentPosition;
// [/SL:KB]

    std::string     mToolTipMsg;
    std::string     mParcelNameMsg;
    std::string     mParcelSalePriceMsg;
    std::string     mParcelSaleAreaMsg;
    std::string     mParcelOwnerMsg;
    std::string     mRegionNameMsg;
    std::string     mToolTipHintMsg;
    std::string     mAltToolTipHintMsg;

    // <FS:Ansariel> Mark avatar feature
    typedef std::map<LLUUID, LLColor4> avatar_marks_map_t;
    static avatar_marks_map_t sAvatarMarksMap;

public:
    void            setSelected(uuid_vec_t uuids) { gmSelected=uuids; };
// <FS:CR> Minimap improvements
    void            handleShowProfile(const LLSD& sdParam) const;
    uuid_vec_t      mClosestAgentsToCursor;
    LLUUID          mClosestAgentRightClick;
    uuid_vec_t      mClosestAgentsRightClick;
// </FS:CR>

private:
    bool isZoomChecked(const LLSD& userdata);
    void setZoom(const LLSD& userdata);
    void handleStopTracking(const LLSD& userdata);
    void activateCenterMap(const LLSD& userdata);
    bool isMapOrientationChecked(const LLSD& userdata);
    void setMapOrientation(const LLSD& userdata);
    void popupShowAboutLand(const LLSD& userdata);
    void handleStartTracking();
    void handleMark(const LLSD& userdata);
    void handleClearMark();
    void handleClearMarks();
    void handleCam();
// <FS:Chanayane> Compass floater
    void handleFaceTowards();
    bool canFaceTowards();
// </FS:Chanayane>
// [SL:KB] - Patch: World-MiniMap | Checked: 2012-07-08 (Catznip-3.3)
    void handleOverlayToggle(const LLSD& sdParam);
    void setAvatarProfileLabel(const LLUUID& av_id, const LLAvatarName& avName, const std::string& item_name);
    typedef std::map<LLUUID, boost::signals2::connection> avatar_name_cache_connection_map_t;
    avatar_name_cache_connection_map_t mAvatarNameCacheConnections;
// [/SL:KB]

    bool canAddFriend();
    bool canRemoveFriend();
    bool canCall();
    bool canMap();
    bool canShare();
    bool canOfferTeleport();
    bool canBlock();
    bool canFreezeEject();
    bool canKickTeleportHome();
    bool isBlocked();
    // <FS:Ansariel> Extra request teleport
    bool canRequestTeleport();

    void handleAddFriend();
    void handleAddToContactSet();
    void handleRemoveFriend();
    void handleIM();
    void handleCall();
    void handleMap();
    void handleShare();
    void handlePay();
    void handleOfferTeleport();
    void handleRequestTeleport();
    void handleTeleportToAvatar();
    void handleGroupInvite();
    void handleGetScriptInfo();
    void handleBlockUnblock();
    void handleReport();
    void handleFreeze();
    void handleEject();
    void handleKick();
    void handleTeleportHome();
    void handleEstateBan();
    void handleDerender(bool permanent);
    void handleDisconnectRegion();
    void handleReconnectRegion();

    LLHandle<LLView> mPopupMenuHandle;
    uuid_vec_t      gmSelected;
};


#endif
