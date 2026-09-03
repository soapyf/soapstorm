/**
 * @file ssfloaterassets.h
 * @brief Atmo Magic: global assets floater.
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

#ifndef SS_FLOATERASSETS_H
#define SS_FLOATERASSETS_H

#include "ssprecippreset.h"

#include "llfloater.h"

class SSFloaterAssets : public LLFloater
{
public:
    SSFloaterAssets(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    static std::string stepWidgetName(SSStepSurface surface, SSStepAction action);

    void onCommitWind();
    void onCommitThunder();
    void onCommitSteps();
};

#endif
