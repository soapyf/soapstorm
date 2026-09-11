/**
 * @file ssfloaternavmesh.cpp
 * @brief See ssfloaternavmesh.h.
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

#include "ssfloaternavmesh.h"

#include "ssnavmesh.h"
#include "ssworldfieldshapes.h"

#include "llagent.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llselectmgr.h"
#include "llviewerobject.h"
#include "llvector4a.h"
#include "llworld.h"
#include "pipeline.h"
#include "ssworldfield.h"
#include "llframetimer.h"
#include "lltextbox.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"

SSFloaterNavMesh::SSFloaterNavMesh(const LLSD& key) :
    LLFloater(key)
{
}

// Wires the buttons; the View tab's switches bind straight to their settings in the XML.
bool SSFloaterNavMesh::postBuild()
{
    // <SS:Nexii> The world field's consumers from here as well: the draw toggle is the pipeline's world-field mask (the Atmo debug panel's switch), the view chooser binds to SSWorldFieldDebugView in the XML. draw() re-reads the mask so a toggle from the other panel shows here. [interaction: SSFloaterAtmoDebug::bindOverlayToggle]
    LLCheckBoxCtrl* field_check = getChild<LLCheckBoxCtrl>("field_overlay_check");
    field_check->setCommitCallback([](LLUICtrl*, const LLSD&) { LLPipeline::toggleRenderDebug(LLPipeline::RENDER_DEBUG_WORLD_FIELD); });
    field_check->set(gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_WORLD_FIELD));
    mNavStatus = getChild<LLTextBox>("navmesh_status");
    mCensusStatus = getChild<LLTextBox>("census_status");
    mPathStatus = getChild<LLTextBox>("path_status");
    mDumpStatus = getChild<LLTextBox>("dump_status");
    getChild<LLButton>("dump_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onDumpSelection(); });
    getChild<LLButton>("mark_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onMarkLocation(); });

    getChild<LLButton>("rebuild_button")->setClickedCallback(
        [](LLUICtrl*, const LLSD&) { SSNavMesh::getInstance()->rebuildAll(); });
    getChild<LLButton>("start_here_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { SSNavMesh::getInstance()->setTestStart(gAgent.getPositionAgent()); onFindPath(); });
    getChild<LLButton>("start_camera_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { SSNavMesh::getInstance()->setTestStart(LLViewerCamera::getInstance()->getOrigin()); onFindPath(); });
    getChild<LLButton>("end_here_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { SSNavMesh::getInstance()->setTestEnd(gAgent.getPositionAgent()); onFindPath(); });
    getChild<LLButton>("end_camera_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { SSNavMesh::getInstance()->setTestEnd(LLViewerCamera::getInstance()->getOrigin()); onFindPath(); });
    getChild<LLButton>("find_path_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onFindPath(); });
    getChild<LLButton>("clear_path_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&)
        {
            SSNavMesh::getInstance()->clearTestPath();
            mPathStatus->setText(getString("path_choose"));
        });
    mPathStatus->setText(getString("path_choose"));
    return true;
}

// Status texts follow the live state; a quarter-second cadence is plenty for a console.
void SSFloaterNavMesh::draw()
{
    getChild<LLCheckBoxCtrl>("field_overlay_check")->set(gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_WORLD_FIELD));
    const F32 now = (F32)LLFrameTimer::getTotalSeconds();
    if (now - mLastRefresh > 0.25f)
    {
        mLastRefresh = now;
        refresh();
    }
    LLFloater::draw();
}

void SSFloaterNavMesh::refresh()
{
    static LLCachedControl<bool> nav_enabled(gSavedSettings, "SSNavMesh", false);
    static LLCachedControl<bool> census_enabled(gSavedSettings, "SSWorldFieldShapes", false);
    SSNavMesh* nav = SSNavMesh::getInstance();
    SSWorldFieldShapes* shapes = SSWorldFieldShapes::getInstance();

    if (!nav_enabled || !census_enabled)
    {
        mNavStatus->setText(getString("status_off"));
    }
    else if (!nav->active())
    {
        mNavStatus->setText(getString("status_no_census"));
    }
    else
    {
        mNavStatus->setText(llformat("%d columns, %d bands (%d to build, %d in flight)\n%u polygons, %.1f MB of layers\n%u builds, last %.1f ms worker + %.1f ms publish\n%d mover obstacles",
                                     nav->columnCount(), nav->bandCount(), nav->pendingCount(), nav->inFlightCount(),
                                     nav->polyCount(), nav->layerBytes() / 1048576.0,
                                     nav->buildCount(), nav->lastBuildMS(), nav->lastPublishMS(), nav->obstacleCount()));
    }

    if (!census_enabled)
    {
        mCensusStatus->setText(getString("census_off"));
    }
    else
    {
        mCensusStatus->setText(llformat("%d records, %d triangles, %d parts cached\n%s, last build %.1f ms\nrest by: pathfinding role %d, ROC ledger %d, watching %d\nnavmesh roles known %d%s, physics shapes asked %d\nlast schedule saw %d records: %d dynamic, %d phantom",
                                        shapes->recordCount(), shapes->triangleCount(), shapes->cachedPartCount(),
                                        shapes->building() ? "rebuilding" : (shapes->censusCurrent() ? "current" : "stale"),
                                        shapes->lastBuildMS(), shapes->restByFlag(), shapes->restByLedger(), shapes->restByWatching(),
                                        shapes->navRoleCount(), shapes->navRolesInFlight() ? " (asking)" : "", shapes->physicsRequested(),
                                        nav->lastScheduleSeen(), nav->lastScheduleDynamic(), nav->lastSchedulePhantom()));
    }
}

// <SS:Nexii> "Why is this ignored": every selected linkset dumped to the log under SSNavMeshDump - the census's inputs and skip rules, its records, the navmesh columns and bands under it - with the root's verdict shown here. [interaction: SSWorldFieldShapes::dumpObject]
void SSFloaterNavMesh::onDumpSelection()
{
    LLObjectSelectionHandle sel = LLSelectMgr::getInstance()->getSelection();
    std::vector<std::string> lines;
    std::string verdict;
    S32 roots = 0;
    for (LLObjectSelection::root_iterator it = sel->root_begin(); it != sel->root_end(); ++it)
    {
        LLViewerObject* obj = (*it)->getObject();
        if (!obj) continue;
        ++roots;
        const std::string v = SSWorldFieldShapes::getInstance()->dumpObject(obj, lines);
        if (verdict.empty()) verdict = v;
    }
    if (roots == 0)
    {
        LLViewerObject* obj = sel->getFirstObject();
        if (obj) { ++roots; verdict = SSWorldFieldShapes::getInstance()->dumpObject(obj, lines); }
    }
    if (roots == 0)
    {
        mDumpStatus->setText(getString("dump_none"));
        return;
    }
    for (const std::string& line : lines) LL_INFOS("SSNavMeshDump") << line << LL_ENDL;
    mDumpStatus->setText(llformat("%d linkset(s), %d lines in the log under SSNavMeshDump.\n%s", roots, (S32)lines.size(), verdict.c_str()));
}

// <SS:Nexii> "What is wrong here": a ray from the camera centre finds the first object or the land, and the hit is dumped from every side - the object's own census dump, the records meeting a 3 m box, the world field column, the navmesh columns and bands, and every portal edge within 4 m with its link state. The overlay draws a magenta post at the mark. [interaction: SSNavMesh::dumpLinksAt]
void SSFloaterNavMesh::onMarkLocation()
{
    const LLVector3 origin = LLViewerCamera::getInstance()->getOrigin();
    const LLVector3 dir = LLViewerCamera::getInstance()->getAtAxis();
    const LLVector3 far_end = origin + dir * 512.f;
    LLVector4a start, end, hit4;
    start.load3(origin.mV);
    end.load3(far_end.mV);
    S32 face = -1;
    LLViewerObject* hit_obj = gPipeline.lineSegmentIntersectInWorld(start, end, false, false, true, false, &face, nullptr, nullptr, &hit4);
    LLVector3 hit;
    bool have = false;
    if (hit_obj)
    {
        hit.set(hit4.getF32ptr());
        have = true;
        if (hit_obj->getPCode() == LLViewerObject::LL_VO_SURFACE_PATCH) hit_obj = nullptr;     // the land, hit as its patch object
    }
    // The land, when it is nearer than any object: a half-metre march down the ray.
    const F32 limit = have ? (hit - origin).magVec() : 512.f;
    for (F32 t = 0.5f; t < limit; t += 0.5f)
    {
        const LLVector3 p = origin + dir * t;
        if (LLWorld::getInstance()->getRegionFromPosAgent(p) && p.mV[VZ] <= LLWorld::getInstance()->resolveLandHeightAgent(p))
        {
            hit = p; have = true; hit_obj = nullptr;
            break;
        }
    }
    if (!have)
    {
        mDumpStatus->setText(getString("mark_none"));
        return;
    }
    std::vector<std::string> lines;
    lines.push_back(llformat("=== mark at (%.2f, %.2f, %.2f) from the camera at (%.1f, %.1f, %.1f): %s", hit.mV[VX], hit.mV[VY], hit.mV[VZ],
                             origin.mV[VX], origin.mV[VY], origin.mV[VZ],
                             hit_obj ? llformat("object %s face %d", hit_obj->getID().asString().c_str(), face).c_str() : "the land"));
    std::string verdict = "the land";
    if (hit_obj) verdict = SSWorldFieldShapes::getInstance()->dumpObject(hit_obj, lines);
    lines.push_back("--- census records within 3 m");
    SSWorldFieldShapes::getInstance()->dumpRecordsAt(hit - LLVector3(3.f, 3.f, 3.f), hit + LLVector3(3.f, 3.f, 3.f), lines);
    lines.push_back("--- world field column");
    if (SSWorldField::instanceExists()) SSWorldField::getInstance()->dumpColumn(hit, lines);
    lines.push_back("--- navmesh");
    SSNavMesh::getInstance()->dumpAt(hit - LLVector3(2.f, 2.f, 2.f), hit + LLVector3(2.f, 2.f, 2.f), lines);
    SSNavMesh::getInstance()->dumpLinksAt(hit, 4.f, lines);
    SSNavMesh::getInstance()->setMark(hit);
    for (const std::string& line : lines) LL_INFOS("SSNavMeshDump") << line << LL_ENDL;
    mDumpStatus->setText(llformat("Marked (%.1f, %.1f, %.1f): %d lines in the log under SSNavMeshDump.\n%s", hit.mV[VX], hit.mV[VY], hit.mV[VZ], (S32)lines.size(), verdict.c_str()));
}

void SSFloaterNavMesh::onFindPath()
{
    SSNavMesh* nav = SSNavMesh::getInstance();
    if (!nav->hasTestStart() || !nav->hasTestEnd())
    {
        mPathStatus->setText(getString("path_choose"));
        return;
    }
    if (!nav->active())
    {
        mPathStatus->setText(getString("path_no_navmesh"));
        return;
    }
    std::string status;
    if (!nav->runTestPath(status))
    {
        mPathStatus->setText(getString("path_none"));
        return;
    }
    mPathStatus->setText(status);
}
