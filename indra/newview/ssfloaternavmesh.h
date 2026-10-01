/**
 * @file ssfloaternavmesh.h
 * @brief Atmo Magic: the census navmesh console - status, overlay switches, a test path.
 *
 *        Laid out after the stock Pathfinding view / test floater: navmesh and
 *        census status on top, a View tab whose switches are the SSNavMeshShow*
 *        settings, and a Test path tab that runs Detour between two chosen points
 *        and draws the result in the overlay (doc/atmo_magic_navmesh.md).
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

#ifndef SS_FLOATERNAVMESH_H
#define SS_FLOATERNAVMESH_H

#include "llfloater.h"

class LLTextBox;

class SSFloaterNavMesh : public LLFloater
{
public:
    SSFloaterNavMesh(const LLSD& key);

    bool postBuild() override;
    void draw() override;

private:
    // Rewrites the status texts from the live navmesh and census, a few times a second.
    void refresh();
    void onFindPath();
    void onDumpSelection();
    void onMarkLocation();

    LLTextBox* mNavStatus = nullptr;
    LLTextBox* mCensusStatus = nullptr;
    LLTextBox* mPathStatus = nullptr;
    LLTextBox* mDumpStatus = nullptr;
    F32 mLastRefresh = 0.f;
};

#endif
