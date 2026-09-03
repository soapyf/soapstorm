/**
 * @file fsrezqueue.h
 * @brief Rez queue watch: spots the region-wide asset download backlog that
 *        makes the simulator refuse to rez objects, and reports it as a
 *        status line next to the crosshair.
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

#ifndef FS_REZQUEUE_H
#define FS_REZQUEUE_H

#include "stdtypes.h"

// <SS:Nexii> Rez queue watch While a region's asset download queue is backed up, the simulator stops handing out rez permission: weapons stop firing, attachments stop appearing. The sim publishes that queue in its once-per-second SimStats (pending downloads / uploads), so the block can be detected entirely client side - no in-world script needed. Port of the LSL rez queue checker, including its thresholds. Purely observational: it samples stats that already arrive and draws a line above the crosshair, timing the episode from the first pending asset to the last.
class FSRezQueue
{
public:
    // One sim stat sample out of a SimStats message. Non-queue stats are
    // ignored. Called from process_sim_stats.
    static void onSimStat(U32 stat_id, F32 value);

    // Status line above the crosshair: the live warning while the queue
    // blocks rezzing, then a fading "cleared" line with the duration. Drawn
    // in mouselook and OTS only. Called from LLViewerWindow::draw.
    static void draw();
};

#endif // FS_REZQUEUE_H
