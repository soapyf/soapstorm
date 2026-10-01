/**
 * @file ssatmosynconsole.h
 * @brief Atmo Magic: the V7 sync console - every shared-state input the sky and the storm scheduler derive from, as a read-only overlay.
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

#ifndef SS_ATMOSYNCONSOLE_H
#define SS_ATMOSYNCONSOLE_H

#include "llview.h"
#include "ssstormcells.h" // <SS:Nexii> S12: SSStormCells::Interest - the console claims the scheduler only while visible

#include <string>
#include <utility>
#include <vector>

// <SS:Nexii> V7 of doc/atmo_magic_debug_views.md: an SSStatsView sibling (translucent, monospace, read-only, sized to its own content, toggled from Advanced > Consoles) that prints every input the shared sky state is a function of - the seed, the wall clock and the storm epoch it falls in, the applied track's day phase, the drift accumulator with its wrap span and wrap count, the active storm cells by hashed id with age and stage, the hero's origin / motion / closest approach, and - DEBUG - a vortex row block (kind, hero-parent flag, condensation, live intensity, multi-vortex N, funnel flag, contact global XY per active vortex; dust devil origins; the tornado why-not readout). Two clients screenshot this side by side and a determinism divergence is localisable to one row. Stackable with the other consoles and with an info view. Everything shown is a const getter on state the systems already hold; opening it changes nothing - it never ticks the scheduler, never builds, never selects. [interaction: SSAtmoMagic seed/sharedTime] [interaction: SSAtmoEnvApplier drift/profile] [interaction: SSStormCells cells/hero/whyNot] [interaction: SSVortices active/dustDevils/whyNot]
class SSAtmoSyncConsole : public LLView
{
public:
    struct Params : public LLInitParam::Block<Params, LLView::Params>
    {
        Params()
        {
            changeDefault(mouse_opaque, false);
        }
    };

    SSAtmoSyncConsole(const Params& p);
    virtual ~SSAtmoSyncConsole();

    void draw() override;

    // <SS:Nexii> S12: claims/releases SSStormCells' Interest as the console shows/hides, so the storm scheduler
    // does not pay its per-frame pipeline while this console is closed.
    void onVisibilityChange(bool new_visibility) override;

private:
    // One accumulated line plus whether to draw it dim (a section whose source is absent).
    void line(const std::string& text, bool dim = false);
    void blank();

    std::vector<std::pair<std::string, bool> > mLines;
    SSStormCells::Interest mStormInterest;
};

extern SSAtmoSyncConsole* gSSAtmoSyncConsole;

#endif // SS_ATMOSYNCONSOLE_H
