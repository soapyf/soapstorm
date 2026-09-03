/**
 * @file ssfloateratmodebug.cpp
 * @brief See ssfloateratmodebug.h.
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

#include "ssfloateratmodebug.h"

#include "llcheckboxctrl.h"
#include "lluictrl.h"
#include "pipeline.h"

SSFloaterAtmoDebug::SSFloaterAtmoDebug(const LLSD& key) :
    LLFloater(key)
{
}

// Wires the seven overlay switches to their render debug masks; everything
// else on these tabs binds itself in the XML through control_name.
bool SSFloaterAtmoDebug::postBuild()
{
    bindOverlayToggle("flow_overlay_check", LLPipeline::RENDER_DEBUG_WIND_FLOW);
    bindOverlayToggle("shadow_overlay_check", LLPipeline::RENDER_DEBUG_RAIN_SHADOW);
    bindOverlayToggle("runoff_overlay_check", LLPipeline::RENDER_DEBUG_ROOF_RUNOFF);
    bindOverlayToggle("surface_overlay_check", LLPipeline::RENDER_DEBUG_SURFACE_FIELD);
    bindOverlayToggle("field_overlay_check", LLPipeline::RENDER_DEBUG_WORLD_FIELD);
    bindOverlayToggle("cloud_overlay_check", LLPipeline::RENDER_DEBUG_CLOUD_FIELD);
    bindOverlayToggle("settle_overlay_check", LLPipeline::RENDER_DEBUG_GEOM_SETTLE);
    return true;
}

// Re-reads the masks on open, in case a viewer command or the fast timers UI
// moved one while the floater was closed.
void SSFloaterAtmoDebug::onOpen(const LLSD& key)
{
    for (const auto& binding : mOverlayBindings)
    {
        getChild<LLCheckBoxCtrl>(binding.first)->set(gPipeline.hasRenderDebugMask(binding.second));
    }
}

void SSFloaterAtmoDebug::bindOverlayToggle(const std::string& name, U64 mask)
{
    LLCheckBoxCtrl* check = getChild<LLCheckBoxCtrl>(name);
    check->setCommitCallback(
        [mask](LLUICtrl*, const LLSD&) { LLPipeline::toggleRenderDebug(mask); });
    check->set(gPipeline.hasRenderDebugMask(mask));
    mOverlayBindings.emplace_back(name, mask);
}
