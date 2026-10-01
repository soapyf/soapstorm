/**
 * @file ssatmosynconsole.cpp
 * @brief See ssatmosynconsole.h.
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

#include "ssatmosynconsole.h"

#include "llfontgl.h"
#include "llgl.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llui.h"

#include "ssatmoenvapplier.h"
#include "ssatmoenvasset.h"
#include "ssatmoenvmanager.h"
#include "ssatmoinfoviewcore.h" // <SS:Nexii> DEBUG: vortexKindLabel - the same taxonomy names V2's icons print, so the console and the in-world view never disagree on a kind's spelling
#include "ssatmomagic.h"
#include "ssstormcellcore.h"
#include "ssstormcells.h"
#include "ssvolcloud.h"
#include "ssvortexcore.h"
#include "ssvortices.h"

SSAtmoSyncConsole* gSSAtmoSyncConsole = NULL;

static LLDefaultChildRegistry::Register<SSAtmoSyncConsole> r_ss_atmo_sync_console("ss_atmo_sync_console");

namespace
{
    const S32 PAD = 6;
    const S32 MAX_CELL_ROWS = 12;
    const S32 MAX_VORTEX_ROWS = SSVortex::MAX_ACTIVE; // never truncates - active() is already capped at this
    const S32 MAX_DUST_ROWS = 12;

    const char* stageName(SSStormCell::EStage s)
    {
        switch (s)
        {
            case SSStormCell::STAGE_TCU:      return "TCU   ";
            case SSStormCell::STAGE_MATURING: return "MATUR ";
            case SSStormCell::STAGE_MATURE:   return "MATURE";
            case SSStormCell::STAGE_ANVIL:    return "ANVIL ";
            case SSStormCell::STAGE_DECAY:    return "DECAY ";
        }
        return "?";
    }

    std::string idHex(U64 id)
    {
        return llformat("%08x%08x", (U32)(id >> 32), (U32)(id & 0xffffffffu));
    }
}

SSAtmoSyncConsole::SSAtmoSyncConsole(const Params& p)
:   LLView(p)
{
}

SSAtmoSyncConsole::~SSAtmoSyncConsole()
{
}

// <SS:Nexii> S12: hold the storm scheduler's Interest exactly while this console is visible - toggling it via
// Advanced > Consoles (llviewermenu.cpp) claims/releases automatically through this hook. [interaction: SSStormCells::claim]
void SSAtmoSyncConsole::onVisibilityChange(bool new_visibility)
{
    LLView::onVisibilityChange(new_visibility);
    mStormInterest = new_visibility ? SSStormCells::getInstance()->claim() : SSStormCells::Interest();
}

void SSAtmoSyncConsole::line(const std::string& text, bool dim)
{
    mLines.push_back(std::make_pair(text, dim));
}

void SSAtmoSyncConsole::blank()
{
    mLines.push_back(std::make_pair(std::string(), false));
}

// Gathers the frame's shared inputs into lines, then draws them SSStatsView-style. Every read is a const getter; the instanceExists guards keep the console from constructing a singleton it only wants to look at.
void SSAtmoSyncConsole::draw()
{
    mLines.clear();

    line("ATMO SYNC  (shared-state inputs; compare two clients row by row)");

    // ---- Seed and clock ------------------------------------------------------ [interaction: SSAtmoMagic seed/sharedTime/hasWeather]
    if (!SSAtmoMagic::instanceExists())
    {
        line("  atmo: not running", true);
    }
    else
    {
        const SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
        // <SS:Nexii> D4 (7e audit, 2026-09-06): SSStormCells::now() IS cycle time (tau, phase 8 section 2), not the
        // raw wall clock - under the editor's preview it can sit hours away from sharedTime(). This row is labelled
        // "cycle tau" (not "wall") and prints the wall second beside it for exactly that reason: the STORM CELLS
        // section below resolved against tau, so two clients (or a scrubbed preview against the real clock) compare
        // row by row on tau, while the wall second alongside shows how far preview has pulled it from real time.
        // Falls back to sharedTime() for both when the scheduler is not driving (no track/region/asset), same as
        // before. [interaction: SSStormCells::now]
        const F64 tau = (SSStormCells::instanceExists() && SSStormCells::getInstance()->valid())
                         ? SSStormCells::getInstance()->now() : atmo->sharedTime();
        const F64 wall_now = atmo->sharedTime();
        const S64 epoch = SSStormCell::epochOf(tau);
        const F64 into = tau - (F64)epoch * SSStormCell::EPOCH_S;
        line(llformat("  seed      0x%08x", atmo->seed()));
        line(llformat("  cycle tau %.3f s  (wall %.3f s)   storm epoch %lld  +%.1f s of %.0f", tau, wall_now, (long long)epoch, into, SSStormCell::EPOCH_S));
        line(llformat("  weather   %s  precip %.2f  temp %.1f C", atmo->hasWeather() ? "cube applied" : "none", atmo->precipitation(), atmo->temperatureC()),
             !atmo->hasWeather());
    }

    // ---- Track, phase, drift ------------------------------------------------- [interaction: SSAtmoEnvApplier appliedTrackIndex/appliedPhase/drift/windProfile] [interaction: SSAtmoEnvManager asset/hasPreviewPhaseOverride]
    if (!SSAtmoEnvApplier::instanceExists() || !SSAtmoEnvManager::instanceExists())
    {
        line("  applier: not running", true);
    }
    else
    {
        const SSAtmoEnvApplier& applier = SSAtmoEnvApplier::instance();
        const SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
        const S32 track = applier.appliedTrackIndex();
        if (track < 0 || !mgr->hasAsset() || track >= (S32)mgr->asset().mTracks.size())
        {
            line("  track     none applied", true);
        }
        else
        {
            const SSAtmoEnvTrack& t = mgr->asset().mTracks[(size_t)track];
            line(llformat("  track     %d '%s'  day %.0f s  offset %.0f s%s", track, t.mName.c_str(), t.mDayLengthSeconds, t.mDayOffsetSeconds,
                          mgr->hasPreviewPhaseOverride() ? "  PREVIEW OVERRIDE" : ""));
            line(llformat("  phase     %.6f  (of one day cycle)", applier.appliedPhase()));
        }

        const LLVector2& drift = applier.cloudDriftMetres();
        const LLVector2 cirrus = applier.cirrusDriftMetres();
        line(llformat("  drift     base (%.2f, %.2f) m   wrap span %.0f m   wraps %u", drift.mV[0], drift.mV[1], applier.driftWrapSpanM(), applier.driftWrapCount()));
        line(llformat("  cirrus    (%.2f, %.2f) m   base z %.1f  floor z %.1f  cirrus z %.1f", cirrus.mV[0], cirrus.mV[1],
                      applier.windProfileBaseZ(), applier.windProfileGroundZ(), applier.cirrusAltitudeMetres()));

        const SSWindProfile::Params& p = applier.windProfile();
        line(llformat("  wind      curve-resolved 10m %.2f m/s @ %05.1f  S %.3f  veer %.1f  exp %.3f  anvil agl %.0f",
                      p.mSpeed10MS, p.mHeading10Deg, p.mShearStrength, p.mVeerDeg, p.mExponent, p.mAnvilAglM));
    }

    // [interaction: SSVolCloud cloudBaseZ/cloudTopZ/noiseTileMetres]
    if (SSVolCloud::instanceExists())
    {
        const SSVolCloud* clouds = SSVolCloud::getInstance();
        if (clouds->empty())
        {
            line("  deck      not built", true);
        }
        else
        {
            line(llformat("  deck      base z %.1f  top z %.1f  noise tile %.0f m", clouds->cloudBaseZ(), clouds->cloudTopZ(), clouds->noiseTileMetres()));
        }
    }

    blank();

    // ---- Storm cells ----------------------------------------------------------- [interaction: SSStormCells cells/hero/whyNot/anchor/latticeCount/currentEpoch]
    if (!SSStormCells::instanceExists() || !SSStormCells::getInstance()->valid())
    {
        line("STORM CELLS  not running (no track, region or asset)", true);
    }
    else
    {
        const SSStormCells* cells = SSStormCells::getInstance();
        const SSStormCells::WhyNot& why = cells->whyNot();
        line(llformat("STORM CELLS  anchor (%.1f, %.1f) global  lattice %d  epoch %lld", cells->anchor().x, cells->anchor().y,
                      cells->latticeCount(), (long long)cells->currentEpoch()));
        line(llformat("  window    %d candidates  %d alive  %d spawned  %d super  %d tornado-eligible  next epoch %.1f min",
                      why.mCandidates, why.mAlive, why.mSpawned, why.mSupercells, why.mTornadoEligible, why.mNextEpochInS / 60.0));

        const std::vector<SSStormCells::ActiveCell>& list = cells->cells();
        if (list.empty())
        {
            line("  cells     none active", true);
        }
        else
        {
            line("  id                age s   age01  stage   centre global (x, y)         r m    P     rot    I");
            S32 shown = 0;
            for (const SSStormCells::ActiveCell& c : list)
            {
                if (shown++ >= MAX_CELL_ROWS)
                {
                    line(llformat("  ... %d more (ranked by id)", (S32)list.size() - MAX_CELL_ROWS), true);
                    break;
                }
                const F64 age = cells->now() - c.mCandidate.mBirthTime;
                line(llformat("  %s %s %6.0f  %.3f  %s  (%10.1f, %10.1f)  %5.0f  %.2f  %+.2f  %.2f",
                              idHex(c.mCandidate.mId).c_str(), c.mIsHero ? "H" : (c.mGate.mSupercell ? "S" : " "), age, c.mAge01,
                              stageName(c.mLifecycle.mStage), c.mCentre.x, c.mCentre.y, c.mRadiusM,
                              c.mCandidate.mPotential, c.mCandidate.mRotation, c.mGate.mIntensity));
            }
        }

        blank();
        const SSStormCells::Hero* hero = cells->hero();
        if (!hero)
        {
            line("HERO  none", true);
            for (const std::string& f : why.mFailing)
            {
                line("  why not: " + f, true);
            }
        }
        else
        {
            const F64 now = cells->now();
            const SSStormCell::Vec2 centre = SSStormCell::centreAt(hero->mPath.mOrigin, hero->mPath.mMotion, hero->mBirthTime, now);
            line(llformat("HERO  %s  birth %.3f  life %.0f s  age %.0f s", idHex(hero->mId).c_str(), hero->mBirthTime, hero->mLifetimeS, now - hero->mBirthTime));
            line(llformat("  origin    (%.1f, %.1f)   motion (%.3f, %.3f) m/s  %.2f m/s", hero->mPath.mOrigin.x, hero->mPath.mOrigin.y,
                          hero->mPath.mMotion.x, hero->mPath.mMotion.y,
                          std::sqrt(hero->mPath.mMotion.x * hero->mPath.mMotion.x + hero->mPath.mMotion.y * hero->mPath.mMotion.y)));
            line(llformat("  now       (%.1f, %.1f)", centre.x, centre.y));
            line(llformat("  closest   (%.1f, %.1f)  %.1f m from anchor  at %.3f  (%+.0f s from now)", hero->mPath.mClosest.x, hero->mPath.mClosest.y,
                          hero->mPath.mClosestDistM, hero->mPath.mClosestTime, hero->mPath.mClosestTime - now));
            line(llformat("  death     (%.1f, %.1f)  at %.3f", hero->mDeath.x, hero->mDeath.y, hero->mBirthTime + (F64)hero->mLifetimeS));
        }
    }

    blank();

    // ---- Vortices (DEBUG task: a vortex row block) ------------------------------ [interaction: SSVortices active/dustDevils/whyNot]
    if (!SSVortices::instanceExists() || !SSVortices::getInstance()->valid())
    {
        line("VORTICES  not running (storm scheduler not valid)", true);
    }
    else
    {
        const SSVortices* vortices = SSVortices::getInstance();
        const std::vector<SSVortices::LiveVortex>& active = vortices->active();
        line(llformat("VORTICES  %d active (of %d max)  %d dust devils", (S32)active.size(), SSVortex::MAX_ACTIVE, (S32)vortices->dustDevils().size()));
        if (active.empty())
        {
            line("  none active", true);
        }
        else
        {
            line("  id                kind          hero cond  I     N  funnel  parent            contact global (x, y)");
            S32 shown = 0;
            for (const SSVortices::LiveVortex& v : active)
            {
                if (shown++ >= MAX_VORTEX_ROWS)
                {
                    line(llformat("  ... %d more", (S32)active.size() - MAX_VORTEX_ROWS), true);
                    break;
                }
                line(llformat("  %s  %-12s  %s   %.2f  %.2f  %d  %s     %s  (%10.1f, %10.1f)%s",
                              idHex(v.mCandidate.mId).c_str(), SSAtmoInfoViewCore::vortexKindLabel((S32)v.mKind), v.mParentIsHero ? "H" : " ",
                              v.mState.mCondensation, v.mState.mIntensity, v.mCandidate.mMultiN, v.mHasFunnel ? "yes" : "no ",
                              idHex(v.mParentId).c_str(), v.mContactGlobal.x, v.mContactGlobal.y,
                              v.mWasRelabelledWaterspout ? "  (relabelled waterspout)" : ""));
            }
        }

        const std::vector<SSVortices::DustVortex>& dust = vortices->dustDevils();
        if (!dust.empty())
        {
            line("  dust      id                origin global (x, y)          I");
            S32 shown = 0;
            for (const SSVortices::DustVortex& dv : dust)
            {
                if (shown++ >= MAX_DUST_ROWS)
                {
                    line(llformat("  ... %d more", (S32)dust.size() - MAX_DUST_ROWS), true);
                    break;
                }
                line(llformat("            %s  (%10.1f, %10.1f)  %.2f", idHex(dv.mCandidate.mId).c_str(),
                              dv.mCandidate.mOriginXY.x, dv.mCandidate.mOriginXY.y, dv.mIntensity));
            }
        }

        const SSVortices::WhyNotTornado& vwhy = vortices->whyNot();
        if (!vwhy.mAlive)
        {
            line(llformat("  TORNADO WHY NOT  hero %s  kind %s  meso %.2f  potential %.2f  super %s  eligible %s  allowTornadoes %s",
                          vwhy.mHaveHero ? idHex(vwhy.mHeroId).c_str() : "none", SSAtmoInfoViewCore::vortexKindLabel((S32)vwhy.mKind),
                          vwhy.mMeso, vwhy.mPotential, vwhy.mSupercell ? "yes" : "no", vwhy.mTornadoEligible ? "yes" : "no",
                          vwhy.mAllowTornadoes ? "on" : "off"), true);
            for (const std::string& f : vwhy.mFailing)
            {
                line("    why not: " + f, true);
            }
        }
    }

    // ---- Draw ------------------------------------------------------------------
    LLFontGL* font = LLFontGL::getFontMonospace();
    if (!font) return;

    const S32 lh = font->getLineHeight();
    S32 widest = 0;
    for (const auto& l : mLines)
    {
        widest = llmax(widest, font->getWidth(l.first));
    }
    const S32 needed_w = widest + PAD * 2;
    const S32 needed_h = (S32)mLines.size() * lh + PAD * 2;

    // Sized to content each frame, anchored at its top-left like the texture console.
    const LLRect r = getRect();
    if (r.getWidth() != needed_w || r.getHeight() != needed_h)
    {
        LLRect nr = r;
        nr.mRight = nr.mLeft + needed_w;
        nr.mBottom = nr.mTop - needed_h;
        setRect(nr);
    }

    gl_rect_2d(0, needed_h, needed_w, 0, LLColor4(0.f, 0.f, 0.f, 0.6f));

    static const LLColor4 normal(1.f, 1.f, 1.f, 1.f);
    static const LLColor4 dim(0.65f, 0.65f, 0.65f, 1.f);
    S32 y = needed_h - PAD;
    for (const auto& l : mLines)
    {
        if (!l.first.empty())
        {
            font->renderUTF8(l.first, 0, PAD, y, l.second ? dim : normal, LLFontGL::LEFT, LLFontGL::TOP);
        }
        y -= lh;
    }
}
