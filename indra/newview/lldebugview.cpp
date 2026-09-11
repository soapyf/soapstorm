/**
 * @file lldebugview.cpp
 * @brief A view containing UI elements only visible in build mode.
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

#include "lldebugview.h"

// library includes
#include "llfasttimerview.h"
#include "llconsole.h"
#include "lltextureview.h"
#include "ssstatsview.h" // <SS:Nexii>
#include "ssatmoinfoview.h" // <SS:Nexii> Atmo Magic info views: dim quad + legend
#include "ssatmosynconsole.h" // <SS:Nexii> Atmo Magic V7 sync console
#include "llresmgr.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llappviewer.h"
#include "llsceneview.h"
#include "llviewertexture.h"
#include "llscenemonitor.h"
//
// Globals
//

LLDebugView* gDebugView = NULL;

//
// Methods
//
static LLDefaultChildRegistry::Register<LLDebugView> r("debug_view");

LLDebugView::LLDebugView(const LLDebugView::Params& p)
:   LLView(p),
    mDebugConsolep(NULL),
    mFloaterSnapRegion(NULL)
{}

LLDebugView::~LLDebugView()
{
    // These have already been deleted.  Fix the globals appropriately.
    gDebugView = NULL;
    gTextureView = NULL;
    gSceneView = NULL;
    gSceneMonitorView = NULL;
}

void LLDebugView::init()
{
    LLRect r;
    LLRect rect = getLocalRect();

    // Rectangle to draw debug data in (full height, 3/4 width)
    r.set(10, rect.getHeight() - 100, ((rect.getWidth()*3)/4), 100);
    LLConsole::Params cp;
    cp.name("debug console");
    cp.max_lines(20);
    cp.rect(r);
    cp.font(LLFontGL::getFontMonospace());
    cp.follows.flags(FOLLOWS_BOTTOM | FOLLOWS_LEFT);
    cp.visible(false);
    mDebugConsolep = LLUICtrlFactory::create<LLConsole>(cp);
    addChild(mDebugConsolep);

    r.set(150 - 25, rect.getHeight() - 50, rect.getWidth()/2 - 25, rect.getHeight() - 450);

    r.setLeftTopAndSize(25, rect.getHeight() - 50, (S32) (gViewerWindow->getWindowRectScaled().getWidth() * 0.75f),
                                     (S32) (gViewerWindow->getWindowRectScaled().getHeight() * 0.75f));

    gSceneView = new LLSceneView(r);
    gSceneView->setFollowsTop();
    gSceneView->setFollowsLeft();
    gSceneView->setVisible(false);
    addChild(gSceneView);
    gSceneView->setRect(rect);

    gSceneMonitorView = new LLSceneMonitorView(r);
    gSceneMonitorView->setFollowsTop();
    gSceneMonitorView->setFollowsLeft();
    gSceneMonitorView->setVisible(false);
    addChild(gSceneMonitorView);
    gSceneMonitorView->setRect(rect);

    // <FS:Ansariel> Fix texture console width
    //r.set(150, rect.getHeight() - 50, 835, 100);
    r.set(150, rect.getHeight() - 60, 965, 100);
    // </FS:Ansariel>
    LLTextureView::Params tvp;
    tvp.name("gTextureView");
    tvp.rect(r);
    tvp.follows.flags(FOLLOWS_TOP|FOLLOWS_LEFT);
    tvp.visible(false);
    gTextureView = LLUICtrlFactory::create<LLTextureView>(tvp);
    addChild(gTextureView);

    // <SS:Nexii> Soapstorm stats overlay, sized to its own content each frame so it starts small and grows only as far as its longest line.
    // Parked just past the right edge of the texture console instead of sharing its top left corner, because the two are siblings of this one debug view and the child added last is drawn last, so an overlay opened over the console silently painted the console's own header out.
    const S32 ss_stats_left = r.mRight + 10;
    const S32 ss_stats_top = r.mTop;
    r.set(ss_stats_left, ss_stats_top, ss_stats_left + 700, ss_stats_top - 100);
    SSStatsView::Params ssp;
    ssp.name("gSSStatsView");
    ssp.rect(r);
    ssp.follows.flags(FOLLOWS_TOP|FOLLOWS_LEFT);
    ssp.visible(false);
    ssp.mouse_opaque(false);
    gSSStatsView = LLUICtrlFactory::create<SSStatsView>(ssp);
    addChild(gSSStatsView);

    // <SS:Nexii> Atmo Magic V7 sync console: shares the texture console's top-left corner (the two are rarely wanted together) and sizes itself to content each frame, so it stacks beside the stats overlay and over an info view without either painting the other out.
    r.set(150, rect.getHeight() - 60, 150 + 600, rect.getHeight() - 60 - 100);
    SSAtmoSyncConsole::Params syncp;
    syncp.name("gSSAtmoSyncConsole");
    syncp.rect(r);
    syncp.follows.flags(FOLLOWS_TOP|FOLLOWS_LEFT);
    syncp.visible(false);
    syncp.mouse_opaque(false);
    gSSAtmoSyncConsole = LLUICtrlFactory::create<SSAtmoSyncConsole>(syncp);
    addChild(gSSAtmoSyncConsole);
    // </SS:Nexii>

    // <SS:Nexii> Atmo Magic info views: the world-dimming quad goes in at the BACK (drawn first, under every console here) and the legend on top; both draw nothing while SSAtmoInfoView is 0.
    SSAtmoInfoView::attach(this);
}

void LLDebugView::draw()
{
    if (mFloaterSnapRegion == NULL)
    {
        mFloaterSnapRegion = gViewerWindow->getFloaterSnapRegion();
    }

    LLRect debug_rect;
    mFloaterSnapRegion->localRectToOtherView(mFloaterSnapRegion->getLocalRect(), &debug_rect, getParent());

    setShape(debug_rect);
    LLView::draw();
}
