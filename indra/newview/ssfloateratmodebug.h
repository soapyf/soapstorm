/**
 * @file ssfloateratmodebug.h
 * @brief Atmo Magic: the debug views floater.
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

#ifndef SS_FLOATERATMODEBUG_H
#define SS_FLOATERATMODEBUG_H

#include "llfloater.h"

#include <utility>
#include <vector>

// Every Atmo Magic debug view, marker and diagnostic overlay in one place,
// under tabs. All of this used to live scattered across the Simulation and
// Effects floaters and the Develop > Render Metadata menu, several switches
// duplicated two or three times over.
class SSFloaterAtmoDebug : public LLFloater
{
public:
    SSFloaterAtmoDebug(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    // The overlay switches drive pipeline render debug masks, which are not
    // settings, so they cannot bind themselves through control_name.
    void bindOverlayToggle(const std::string& name, U64 mask);

    std::vector<std::pair<std::string, U64> > mOverlayBindings;
};

#endif
