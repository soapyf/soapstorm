/**
 * @file fsrezqueue.cpp
 * @brief Rez queue watch: sim pending asset queue tracking and crosshair readout.
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

#include "fsrezqueue.h"

#include "llagentcamera.h"
#include "llfontgl.h"
#include "llframetimer.h"
#include "llrender.h"
#include "llviewercontrol.h"
#include "llviewerstats.h"
#include "llviewerwindow.h"
#include "v4color.h"

// <SS:Nexii> Rez queue watch

// Thresholds and timings carried over from the LSL original.
static const F32 QUEUE_EPSILON = 0.01f; // above this the region has queue activity at all
static const F64 CLEARED_HOLD  = 0.2;   // "cleared" line fully opaque
static const F64 CLEARED_FADE  = 0.3;   // and then fades out
static const F64 STATS_STALE   = 6.0;  // no SimStats for this long: drop the episode (region change, disconnect)

static F32  sPendingDownloads = 0.f;
static F32  sPendingUploads   = 0.f;
static F64  sLastStat         = 0.0;      // arrival time of the last queue stat
static F64  sQueueStart       = 0.0;      // first sample with any queue activity; 0 = idle
static F64  sQueueEnd         = 0.0;      // last sample with any queue activity
static bool sBlocking         = false;    // the queue crossed the rez-blocking threshold
static F64  sClearedAt        = -1000.0;
static F32  sClearedDuration  = 0.f;

// static
void FSRezQueue::onSimStat(U32 stat_id, F32 value)
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSRezQueueIndicator", false);
    if (!enabled) return;

    switch (stat_id)
    {
        case LL_SIM_STAT_PENDING_DOWNLOADS: sPendingDownloads = value; break;
        case LL_SIM_STAT_PENDING_UPLOADS:   sPendingUploads   = value; break;
        default: return;
    }

    static LLCachedControl<F32> threshold(gSavedSettings, "FSRezQueueThreshold", 0.9f);
    const F64 now = LLFrameTimer::getTotalSeconds();

    // Stats stop arriving while crossing regions or reconnecting; an episode
    // held over from before the gap would report a nonsense duration, so drop it.
    if (sQueueStart > 0.0 && now - sLastStat > STATS_STALE)
    {
        sQueueStart = sQueueEnd = 0.0;
        sBlocking = false;
    }
    sLastStat = now;

    // Both stats arrive in the same SimStats message, so evaluating on either
    // one costs at most a message of staleness on the other.
    if (sPendingDownloads > QUEUE_EPSILON || sPendingUploads > QUEUE_EPSILON)
    {
        if (sQueueStart == 0.0)
        {
            sQueueStart = now;
        }
        sQueueEnd = now;
        if (sPendingDownloads >= (F32)threshold)
        {
            sBlocking = true;
        }
    }
    else if (sQueueStart > 0.0)
    {
        // Queue drained. Only the blocking episodes are worth reporting.
        if (sBlocking)
        {
            sClearedAt = now;
            sClearedDuration = (F32)(sQueueEnd - sQueueStart);
        }
        sQueueStart = sQueueEnd = 0.0;
        sBlocking = false;
    }
}

// static
void FSRezQueue::draw()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSRezQueueIndicator", false);
    if (!enabled) return;

    if (!gAgentCamera.cameraMouselook()) return;

    const F64 now = LLFrameTimer::getTotalSeconds();
    std::string text;
    LLColor4 color;

    if (sBlocking && now - sLastStat < STATS_STALE)
    {
        text = llformat("REZ QUEUE %.1fs", (F32)(now - sQueueStart));
        color = LLColor4(1.f, 0.65f, 0.15f, 1.f);
    }
    else
    {
        const F64 cleared_t = now - sClearedAt;
        if (cleared_t < 0.0 || cleared_t >= CLEARED_HOLD + CLEARED_FADE)
        {
            return;
        }
        const F32 alpha = (cleared_t > CLEARED_HOLD) ? 1.f - (F32)((cleared_t - CLEARED_HOLD) / CLEARED_FADE) : 1.f;
        text = llformat("REZ QUEUE ENDED %.1fs", sClearedDuration);
        color = LLColor4(0.4f, 1.f, 0.4f, alpha);
    }

    // Anchored like the hit report text, but above the crosshair rather than below.
    static LLCachedControl<F32> scale_setting(gSavedSettings, "FSHitMarkerScale", 1.0f);
    static LLCachedControl<F32> text_scale_setting(gSavedSettings, "FSHitMarkerTextScale", 1.0f);
    static LLCachedControl<bool> text_bold(gSavedSettings, "FSHitMarkerTextBold", true);
    const F32 scale = llclamp((F32)scale_setting, 0.5f, 3.f);
    const F32 text_scale = llclamp((F32)text_scale_setting, 0.5f, 3.f);
    const S32 center_x = gViewerWindow->getWorldViewRectScaled().getWidth() / 2;
    const S32 center_y = gViewerWindow->getWorldViewRectScaled().getHeight() / 2;

    const LLFontGL* font = LLFontGL::getFontSansSerif();
    const U8 style = text_bold ? LLFontGL::BOLD : LLFontGL::NORMAL;

    gGL.pushMatrix();
    gGL.translatef((F32)center_x, (F32)center_y + 32.f * scale, 0.f);
    gGL.scalef(text_scale, text_scale, 1.f);
    font->renderUTF8(text, 0, 0.f, 0.f, color,
                     LLFontGL::HCENTER, LLFontGL::BOTTOM, style, LLFontGL::DROP_SHADOW_SOFT);
    gGL.popMatrix();
}
