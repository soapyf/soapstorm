/**
 * @file fsanimtimelinectrl.cpp
 * @brief VATs Animator timeline strip (VATs spec 09, stage 6d).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Copyright (c) 2026 Viewport Avatar Toolset contributors
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

#include "fsanimtimelinectrl.h"

#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llrender2dutils.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr S32 PAD = 8;  // room for the first and last diamond
}

S32 FSAnimTimelineCtrl::xOf(double frame) const
{
    const int last = std::max(lastFrame ? lastFrame() : 1, 1);
    return PAD + S32(std::lround(frame / last * (getRect().getWidth() - 2 * PAD)));
}

double FSAnimTimelineCtrl::frameAt(S32 x) const
{
    const int last = std::max(lastFrame ? lastFrame() : 1, 1);
    const double t = double(x - PAD) / std::max(getRect().getWidth() - 2 * PAD, 1);
    return std::clamp(std::round(t * last), 0.0, double(last));
}

void FSAnimTimelineCtrl::draw()
{
    const S32 w = getRect().getWidth(), h = getRect().getHeight();
    gl_rect_2d(0, h, w, 0, LLColor4(0.f, 0.f, 0.f, 0.35f));
    const int last = std::max(lastFrame ? lastFrame() : 1, 1);

    // Ruler: a tick per frame when there is room, labels about every 50 px.
    const F32 px_per_frame = F32(w - 2 * PAD) / last;
    const int label_step = std::max(1, int(std::ceil(50.f / std::max(px_per_frame, 0.01f))));
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    for (int f = 0; f <= last; ++f)
    {
        const bool labelled = f % label_step == 0;
        if (!labelled && px_per_frame < 4.f)
            continue;
        const S32 x = xOf(f);
        gl_line_2d(x, h, x, h - (labelled ? 8 : 4), LLColor4(1.f, 1.f, 1.f, labelled ? 0.5f : 0.2f));
        if (labelled)
            font->renderUTF8(std::to_string(f), 0, x + 2, h - 3, LLColor4(1.f, 1.f, 1.f, 0.6f), LLFontGL::LEFT,
                             LLFontGL::TOP);
    }

    // Pins as light-blue bands under the keys (VATs TG-100).
    if (bands)
        for (const auto& [f0, f1] : bands())
            gl_rect_2d(xOf(f0), h - 10, std::max(xOf(f1), xOf(f0) + 2), 2, LLColor4(0.43f, 0.75f, 1.f, 0.28f));

    // Keys as diamonds along the middle.
    const S32 mid = (h - 12) / 2;
    if (keyFrames)
    {
        for (double f : keyFrames())
        {
            const S32 x = xOf(f);
            const LLColor4 amber(0.96f, 0.77f, 0.36f, 1.f);
            gl_triangle_2d(x - 5, mid, x, mid + 5, x + 5, mid, amber, true);
            gl_triangle_2d(x - 5, mid, x + 5, mid, x, mid - 5, amber, true);
        }
    }

    // Playhead.
    if (currentFrame)
    {
        const S32 x = xOf(currentFrame());
        gl_rect_2d(x - 1, h, x + 1, 0, LLColor4(0.93f, 0.44f, 0.63f, 1.f));
    }
    LLUICtrl::draw();
}

bool FSAnimTimelineCtrl::handleMouseDown(S32 x, S32 y, MASK mask)
{
    gFocusMgr.setMouseCapture(this);
    if (onScrub)
        onScrub(frameAt(x));
    return true;
}

bool FSAnimTimelineCtrl::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture() && onScrub)
        onScrub(frameAt(x));
    return LLUICtrl::handleHover(x, y, mask);
}

bool FSAnimTimelineCtrl::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
        gFocusMgr.setMouseCapture(nullptr);
    return true;
}
