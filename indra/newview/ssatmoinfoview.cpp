/**
 * @file ssatmoinfoview.cpp
 * @brief See ssatmoinfoview.h.
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

#include "ssatmoinfoview.h"
#include "ssatmoinfoviewcore.h"
#include "ssdecklodcore.h"
#include "ssdeckmacrocore.h"
#include "ssvirgacore.h"

#include "llappviewer.h" // <SS:Nexii> S12: gFrameCount - the per-frame memo key for stormCellsData()
#include "llfontgl.h"
#include "llgl.h"
#include "llhudrender.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "lluictrlfactory.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h" // <SS:Nexii> V10: regionp->getHandle() for the acoustics probe
#include "llworld.h" // <SS:Nexii> V10: LLWorld::getInstance()->getRegionFromPosAgent for the acoustics probe
#include "llenvironment.h" // <SS:Nexii> info-view look: getLightDirection, the key direction the warm-gray shade is lit from
#include "llviewershadermgr.h" // <SS:Nexii> info-view look: gSSInfoLookProgram
#include "llviewerwindow.h" // <SS:Nexii> gViewerWindow setup3DRender for the look pass and the in-world layer
#include "pipeline.h"
#include "ssinfolookcore.h" // <SS:Nexii> info-view look: LOOK_KEY_ELEVATION, and the formulas the look shader transliterates

#include "ssatmoenvapplier.h"
#include "ssatmoenvasset.h"
#include "ssatmoenvcloudfieldstate.h"
#include "ssatmoenvmanager.h"
#include "ssatmoenvweatherstate.h"
#include "ssatmomagic.h"
#include "sslightning.h"        // <SS:Nexii> V9: SSStrike/SSLightning - the strike list, the scene-light exporter and SSDissolve's plasma window
#include "sslightningrender.h"  // <SS:Nexii> V9: SSLightningRender::stats() - the pass's own last-frame draw counters
#include "ssstormcells.h"
#include "sssquallcore.h"
#include "ssvolcloud.h"
#include "ssvortexcore.h"
#include "ssvortices.h"
#include "sswindflow.h"

#include <map> // <SS:Nexii> V2 lattice-tint smoothing: lx/ly -> potential lookup for the per-corner average (renderStormCells)

static LLDefaultChildRegistry::Register<SSAtmoGraphView> r_ss_atmo_graph_view("ss_atmo_graph_view");

U32  SSAtmoInfoView::sLastMode = 0;
bool SSAtmoInfoView::sFlowMaskWasOn = false;
boost::signals2::scoped_connection SSAtmoInfoView::sModeConnection;

namespace
{
    using namespace SSAtmoInfoViewCore;

    const S32 PAD = 6;
    const S32 CURVE_SAMPLES = 64;
    const S32 CUBE_SAMPLES = 96; // V5 Weather Cube: samples per lane across one full day cycle

    // <SS:Nexii> CHART (design section 6.2): the debug HUD's chart, docked to the legend's right edge - fixed size,
    // 8px gap, bottom-aligned. sLegendHandle is set once by SSAtmoInfoView::attach and read fresh by
    // SSAtmoGraphView::draw() every frame (an LLHandle, not a raw LLView*, so a future teardown of the debug view
    // makes it silently return null rather than dangle). [interaction: SSAtmoLegendView]
    const S32 CHART_W = 440;
    const S32 CHART_H = 280;
    const S32 CHART_GAP = 8;

    // <SS:Nexii> V9: the slot count the deferred lighting pass asks SSLightning::sceneLights for. LOCKSTEP
    // pipeline.cpp renderDeferredLighting's own sceneLights(strike_lights, strike_colors, 4) call - the view asks
    // for the same number so what it draws is the same set the world is actually lit by, not a longer one.
    const S32 SCENE_LIGHT_SLOTS = 4;
    LLHandle<LLView> sLegendHandle;

    // Rail palette: geometry annotations, one colour each, shared by the chart, the legend and the mast so a rail reads the same in every place it appears.
    const LLColor4 RAIL_REF     (0.85f, 0.85f, 0.85f, 0.9f);   // 10 m reference
    const LLColor4 RAIL_BL      (1.00f, 1.00f, 1.00f, 0.9f);   // boundary-layer top
    const LLColor4 RAIL_BASE    (0.35f, 0.55f, 1.00f, 0.9f);   // deck base (moisture blue)
    const LLColor4 RAIL_LID     (0.60f, 0.78f, 1.00f, 0.9f);   // deck lid
    const LLColor4 RAIL_CIRRUS  (1.00f, 0.85f, 0.45f, 0.95f);  // the moving rail
    const LLColor4 CURVE_LIVE   (0.45f, 0.95f, 0.90f, 1.0f);
    const LLColor4 CURVE_FLOW   (0.75f, 0.75f, 0.75f, 0.8f);
    const LLColor4 CURVE_GRAD   (0.50f, 0.50f, 0.50f, 0.8f);
    const LLColor4 AXIS         (0.70f, 0.70f, 0.70f, 0.8f);
    const LLColor4 GRID         (1.00f, 1.00f, 1.00f, 0.10f);
    const LLColor4 TEXT_NORMAL  (1.00f, 1.00f, 1.00f, 1.0f);
    const LLColor4 TEXT_DIM     (0.65f, 0.65f, 0.65f, 1.0f);

    LLColor4 toColor(const RGB& c, F32 a = 1.f)
    {
        return LLColor4(c.r, c.g, c.b, a);
    }

    LLColor4 rampSpeed(F32 t)
    {
        return toColor(speedRamp(t, 1.f));
    }

    LLColor4 rampEnergy(F32 t)
    {
        return toColor(energyRamp(t, 1.f));
    }

    LLColor4 rampGrey(F32 t)
    {
        return toColor(presenceRamp(t, 1.f));
    }

    // V3 palette: one colour per LOD rail, shared by the in-world rings, the legend and the chart.
    const LLColor4 RING_SUBS_FULL  (0.45f, 0.95f, 0.90f, 0.9f);  // SUBS_FULL_M
    const LLColor4 RING_SUBS_TWO   (0.55f, 0.85f, 1.00f, 0.9f);  // SUBS_TWO_M
    const LLColor4 RING_THIN_START (1.00f, 0.85f, 0.45f, 0.9f);  // THIN_START_M
    const LLColor4 RING_FADE_START (1.00f, 0.60f, 0.30f, 0.9f);  // FIELD_FADE_START_M
    const LLColor4 RING_DECK_EDGE  (1.00f, 0.30f, 0.30f, 0.9f);  // DECK_EDGE_M

    // <SS:Nexii> LOD phase 6d (ssdeckmacrocore.h CONTRACT): Tier B's own swatch - the macro grid tint beyond
    // TIER_B_M and the legend's "Tier B macro bodies" line - a violet distinct from every V3 rail so the merged
    // tier reads as its own thing rather than a sixth shade of the same ramp.
    const LLColor4 RING_TIER_B     (0.70f, 0.45f, 1.00f, 0.9f);

    // V4 palette: the fall-tilt comparison line reuses the deck-base rail's own "moisture blue" (RAIL_BASE above)
    // rather than a new swatch - one blue means one thing across V1 and V4. The handoff ring/band get their own
    // white, the same idiom V2's hero-pass rings use for the anchor below.
    const LLColor4 VIRGA_HANDOFF (1.00f, 1.00f, 1.00f, 0.55f);

    // <SS:Nexii> F9 (2026-09-06 review): the curtain's OWN skew line gets its own swatch, distinct from RAIL_BASE's
    // fall-tilt approximation - an amber, so the two comparison lines never read as the same colour meaning two
    // different things.
    const LLColor4 VIRGA_SKEW (1.00f, 0.70f, 0.20f, 0.85f);

    // V2 palette: the anchor and hero marks that are not a ramp colour.
    const LLColor4 ANCHOR_WHITE (1.00f, 1.00f, 1.00f, 0.9f);
    const LLColor4 HERO_ORIGIN  (1.00f, 0.72f, 0.30f, 1.0f);
    const LLColor4 HERO_DEATH   (0.60f, 0.15f, 0.10f, 1.0f);
    const LLColor4 TILE_EDGE    (1.00f, 1.00f, 1.00f, 0.12f);
    const LLColor4 TILE_ALIVE   (1.00f, 1.00f, 1.00f, 0.55f);

    // <SS:Nexii> SQUALL/FORCED palette (doc/atmo_magic_storm_dynamics.md sections 5-6): a bar-and-marker language
    // distinct from every ring/glyph colour already in use, so a line reads as its own thing crossing several
    // cells rather than a property of any one of them. The forced outline reuses HERO_DEATH's dark red at full
    // alpha - an authored cue and a dying hero are both "this cell's fate was decided elsewhere", and the two never
    // appear on the same cell (a forced candidate is never also a line member, per ssstormcellcore.h's own comment).
    const LLColor4 SQUALL_LINE      (0.85f, 0.85f, 1.00f, 0.85f);  // the bar through a line's own members
    const LLColor4 SQUALL_JUNCTION  (1.00f, 0.95f, 0.35f, 0.95f);  // leading-edge QLCS spin-up points
    const LLColor4 FORCED_OUTLINE   (1.00f, 0.20f, 0.75f, 0.95f);  // the authored/forced pin's own outline

    // <SS:Nexii> V2 LINE BAND fills (doc/atmo_magic_phase8_show.md section 3 item 1): the same hue as SQUALL_LINE
    // (a squall line's several visuals should read as one thing) but as translucent FILLS rather than a bar -
    // the wall darker/more opaque than the shelf ahead of it, so the two never get mistaken for each other. Base
    // alpha only; both are scaled by the line's live strength at the draw site.
    const LLColor4 LINE_WALL_FILL  (0.75f, 0.80f, 1.00f, 0.35f);
    const LLColor4 LINE_SHELF_FILL (0.75f, 0.80f, 1.00f, 0.15f);

    // <SS:Nexii> V5 Weather Cube palette: one colour per curve, shared by the top lane's day-cycle curves and the
    // bottom lane's derived gates, plus the "now" cursor and the two cue-marker colours (storm cue vs precipitation
    // cue) - never reusing a V1-V4 swatch, so a colour means one thing on this chart without also meaning "deck
    // lid" or "hero origin" a scroll away.
    const LLColor4 CUBE_MOISTURE    (0.35f, 0.55f, 1.00f, 0.95f);
    const LLColor4 CUBE_CONVECTION  (1.00f, 0.55f, 0.15f, 0.95f);
    const LLColor4 CUBE_TEMP        (1.00f, 0.85f, 0.35f, 0.95f);
    const LLColor4 CUBE_WIND        (0.45f, 0.95f, 0.90f, 0.95f);
    const LLColor4 CUBE_SHEAR       (0.80f, 0.55f, 1.00f, 0.95f);
    const LLColor4 CUBE_CONSOLIDATE (0.90f, 0.90f, 0.90f, 0.95f);
    const LLColor4 CUBE_GLOOM       (0.45f, 0.45f, 0.55f, 0.95f);
    const LLColor4 CUBE_ANVIL       (1.00f, 0.60f, 0.20f, 0.95f);
    const LLColor4 CUBE_LIGHTNING   (0.62f, 0.55f, 1.00f, 0.95f);
    const LLColor4 CUBE_STORM_SCORE (1.00f, 0.30f, 0.30f, 0.95f);
    const LLColor4 CUBE_NOW         (1.00f, 1.00f, 1.00f, 0.9f);
    const LLColor4 CUBE_CUE_STORM   (1.00f, 0.20f, 0.75f, 0.9f);   // matches FORCED_OUTLINE: same authored pin
    const LLColor4 CUBE_CUE_PRECIP  (0.35f, 0.85f, 1.00f, 0.9f);

    // <SS:Nexii> V9 Lightning palette (doc/atmo_magic_debug_views.md V9). The CHANNEL and the stage marks are
    // coloured by the shared lifecycle ramp (SSAtmoInfoViewCore::strikeStageColor - the same cool-to-warm bar V2's
    // cells use), so nothing new is invented for the thing this view is actually about. These are the marks that
    // are NOT a ramp colour: the pending strike's aim line, the attachment cross, the ground crawl, the two light
    // readings (the deferred scene light the world is lit by, and the weaker reading the cloud deck is lit by),
    // and the "occluded" dimming. CUBE_LIGHTNING's violet is deliberately reused for the strike-light marks - it
    // is already "lightning" on V5's own chart, and a colour meaning one thing across two views is the point.
    const LLColor4 STRIKE_AIM       (1.00f, 0.35f, 0.85f, 0.70f);  // origin -> intended attachment, before contact
    const LLColor4 STRIKE_ATTACH    (1.00f, 1.00f, 1.00f, 0.90f);  // the attachment point itself (named ATTACH, not GROUND: sslightning.h already owns STRIKE_GROUND as an SSStrikeKind value)
    const LLColor4 STRIKE_CRAWL     (1.00f, 0.55f, 0.15f, 0.90f);  // the surface crawl run past the foot
    const LLColor4 STRIKE_SCENELIGHT(0.62f, 0.55f, 1.00f, 0.85f);  // sceneLights(): what the deferred pass lights the world with
    const LLColor4 STRIKE_CLOUDLIGHT(0.62f, 0.55f, 1.00f, 0.45f);  // ... and the weaker cut the cloud shader takes
    const LLColor4 STRIKE_OCCLUDED  (0.45f, 0.45f, 0.50f, 0.70f);  // the renderer's occlusion query hid this ground show
    const LLColor4 STRIKE_FIRE_FILL (1.00f, 0.45f, 0.10f, 0.30f);  // ground-fire blob discs (tile-tint fill)

    const char* stageLabel(S32 stage)
    {
        switch (stage)
        {
            case SSStormCell::STAGE_TCU:      return "TCU";
            case SSStormCell::STAGE_MATURING: return "MATURING";
            case SSStormCell::STAGE_MATURE:   return "MATURE";
            case SSStormCell::STAGE_ANVIL:    return "ANVIL";
            case SSStormCell::STAGE_DECAY:    return "DECAY";
        }
        return "?";
    }

    std::string shortId(U64 id)
    {
        return llformat("%08x", (U32)(id & 0xffffffffu));
    }

    // SSVortex::Vec2 and SSStormCell::Vec2 are the same shape in two core namespaces (neither core may include the
    // other's Vec2 - see each core's own file-top include list); every crossing at a read site is this copy, not a
    // cast, same as ssvortices.cpp's own toVortexVec2 does it the other way.
    SSStormCell::Vec2 toStormVec2(const SSVortex::Vec2& v)
    {
        SSStormCell::Vec2 out;
        out.x = v.x;
        out.y = v.y;
        return out;
    }

    const LLColor4 DUST_COLOR (0.85f, 0.70f, 0.35f, 0.9f); // dust devil marker: dusty tan, distinct from the funnel kind colours

    // Orbit period of the rotation glyphs, seconds of WALL CLOCK per revolution (display only).
    const F32 ORBIT_PERIOD_S = 12.f;
    const S32 ORBIT_GLYPHS = 6;

    F32 speedOf(const SSWindProfile::Vec2& v)
    {
        return std::sqrt(v.x * v.x + v.y * v.y);
    }

    // A named horizontal rail of the V1 chart and mast: altitude AGL, label, colour, and whether the data behind it is resident.
    struct Rail
    {
        F32 mAgl;
        const char* mLabel;
        const LLColor4* mColor;
        bool mPresent;
    };

    void collectRails(const SSAtmoInfoView::WindProfileData& d, std::vector<Rail>& out)
    {
        out.push_back({ SSWindProfile::REF_M, "10 m ref", &RAIL_REF, true });
        out.push_back({ SSWindProfile::BL_TOP_M, "1500 m BL top", &RAIL_BL, true });
        out.push_back({ d.mBaseZ - d.mGroundZ, "deck base", &RAIL_BASE, d.mDeckBuilt });
        out.push_back({ d.mLidZ - d.mGroundZ, "deck lid", &RAIL_LID, d.mDeckBuilt });
        out.push_back({ d.mCirrusZ - d.mGroundZ, "cirrus (now)", &RAIL_CIRRUS, true });
    }
}

// ---------------------------------------------------------------------------
// SSAtmoInfoView: mode plumbing
// ---------------------------------------------------------------------------

void SSAtmoInfoView::attach(LLView* debug_view)
{
    if (!debug_view) return;

    LLRect full = debug_view->getLocalRect();

    SSAtmoDimView::Params dp;
    dp.name("ss_atmo_info_dim");
    dp.rect(full);
    dp.follows.flags(FOLLOWS_ALL);
    dp.visible(true);
    dp.mouse_opaque(false);
    SSAtmoDimView* dim = LLUICtrlFactory::create<SSAtmoDimView>(dp);
    // <SS:Nexii> Still added, still in back, but it draws nothing now (the quad moved to the 3-D pass - see SSAtmoDimView::draw): kept so the debug view's child order, and every sibling's z-order with it, is exactly what it was.
    debug_view->addChildInBack(dim);

    LLRect lr;
    lr.set(PAD, PAD + 140, PAD + 280, PAD);
    SSAtmoLegendView::Params lp;
    lp.name("ss_atmo_info_legend");
    lp.rect(lr);
    lp.follows.flags(FOLLOWS_BOTTOM | FOLLOWS_LEFT);
    lp.visible(true);
    lp.mouse_opaque(false);
    SSAtmoLegendView* legend = LLUICtrlFactory::create<SSAtmoLegendView>(lp);
    debug_view->addChild(legend);
    sLegendHandle = legend->getHandle();

    // <SS:Nexii> CHART (design section 6.2): the placeholder rect below is only ever visible for one frame - it
    // reads the legend's own placeholder rect (lr) before the legend's first draw() has sized it to its real
    // content, but SSAtmoGraphView::draw() re-docks itself off the legend's LIVE rect every frame after that, so
    // the fixed starting numbers here never matter beyond that first frame.
    LLRect gr;
    gr.set(lr.mRight + CHART_GAP, lr.mBottom + CHART_H, lr.mRight + CHART_GAP + CHART_W, lr.mBottom);
    SSAtmoGraphView::Params gp;
    gp.name("ss_atmo_info_chart");
    gp.rect(gr);
    gp.follows.flags(FOLLOWS_BOTTOM | FOLLOWS_LEFT);
    gp.visible(true);
    gp.mouse_opaque(false);
    SSAtmoGraphView* chart = LLUICtrlFactory::create<SSAtmoGraphView>(gp);
    debug_view->addChild(chart);

    // The mode drives the engineering masks underneath it: activating a view flips the overlays it hands off to, deactivating restores what was there. The setting does not persist, so a fresh session always starts with the masks untouched.
    LLControlVariable* var = gSavedSettings.getControl("SSAtmoInfoView");
    if (var)
    {
        sLastMode = (U32)var->getValue().asInteger();
        sModeConnection = var->getSignal()->connect(
            [](LLControlVariable*, const LLSD& now, const LLSD&)
            {
                const U32 m = (U32)now.asInteger();
                if (m != sLastMode)
                {
                    onModeChanged(sLastMode, m);
                    sLastMode = m;
                }
            });
    }
}

U32 SSAtmoInfoView::mode()
{
    static LLCachedControl<U32> mode_setting(gSavedSettings, "SSAtmoInfoView", 0);
    return (U32)mode_setting;
}

// The frame's own gate: a mode, or the lightning mask on its own. See the header for why the mask lives here.
bool SSAtmoInfoView::wantsDraw()
{
    return mode() != MODE_OFF || gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_LIGHTNING);
}

namespace
{
    // <SS:Nexii> S12 (phase-2b audit): V2's stake in the storm scheduler running, claimed/released as the mode
    // switches to/from MODE_STORM_CELLS. File-local: only onModeChanged touches it.
    SSStormCells::Interest sStormInterest;
}

// Snapshot the flow-arrow mask on the way from off, then hold it where the active mode wants it; back to off restores the snapshot. V1 hands its near-ground layer to the flowmap arrows, so it wants them on; V2 wants them off under its lattice.
void SSAtmoInfoView::onModeChanged(U32 previous, U32 now)
{
    // S12: claim the storm scheduler only while V2 is the active mode - it should not pay its per-frame pipeline
    // for a view nobody has open. [interaction: SSStormCells::claim]
    sStormInterest = (now == MODE_STORM_CELLS) ? SSStormCells::getInstance()->claim() : SSStormCells::Interest();

    const U64 flow = LLPipeline::RENDER_DEBUG_WIND_FLOW;
    if (previous == MODE_OFF && now != MODE_OFF)
    {
        sFlowMaskWasOn = gPipeline.hasRenderDebugMask(flow);
    }
    const bool desired = (now == MODE_OFF) ? sFlowMaskWasOn : (now == MODE_WIND_PROFILE);
    if (gPipeline.hasRenderDebugMask(flow) != desired)
    {
        LLPipeline::toggleRenderDebug(flow);
    }
}

// Every read here is a const getter on state the systems already hold; the instanceExists guards keep a view from even constructing a singleton, let alone building one.
SSAtmoInfoView::WindProfileData SSAtmoInfoView::windProfileData()
{
    WindProfileData d;
    if (!SSAtmoEnvApplier::instanceExists() || !SSAtmoMagic::instanceExists())
    {
        return d;
    }

    const SSAtmoEnvApplier& applier = SSAtmoEnvApplier::instance();
    d.mValid   = SSAtmoMagic::getInstance()->hasWeather();
    d.mParams  = applier.windProfile();
    d.mGroundZ = applier.windProfileGroundZ();
    d.mBaseZ   = applier.windProfileBaseZ();
    d.mCirrusZ = applier.cirrusAltitudeMetres();

    if (SSVolCloud::instanceExists())
    {
        const SSVolCloud* clouds = SSVolCloud::getInstance();
        d.mDeckBuilt = !clouds->empty();
        if (d.mDeckBuilt)
        {
            d.mLidZ = clouds->cloudTopZ();
        }
    }

    d.mTopAgl = llmax(d.mCirrusZ - d.mGroundZ, SSWindProfile::BL_TOP_M + SSWindProfile::CELL_M);

    F32 vmax = 0.f;
    for (S32 i = 0; i < CURVE_SAMPLES; ++i)
    {
        const F32 agl = altitudeLadder(d.mTopAgl, CURVE_SAMPLES, i);
        vmax = llmax(vmax, speedOf(SSWindProfile::windAt(agl, d.mParams)));
    }
    d.mMaxSpeed = niceMax(vmax);

    if (SSWindFlowMap::instanceExists())
    {
        const SSWindFlowMap* flow = SSWindFlowMap::getInstance();
        d.mFlowAlpha  = flow->windAlpha();
        d.mFlowSolved = flow->windAlphaSolved();
    }
    static LLCachedControl<F32> gradient(gSavedSettings, "SSAtmoWindFlowGradient", 0.16f);
    d.mGradientSetting = llclamp((F32)gradient, SSWindProfile::EXPONENT_MIN, SSWindProfile::EXPONENT_MAX);
    return d;
}

// <SS:Nexii> V3's data: read-only off SSVolCloud's own resident counters (puffCount(), the primary deck's
// mLodSubsTally/mLodCellTally via the accessors added alongside ssdecklodcore.h's wiring) plus the two dials the
// LOD ramp depends on, read straight off gSavedSettings for display exactly as windProfileData() reads
// SSAtmoWindFlowGradient above - UNCLAMPED (SSVolCloud keeps the actual MIN/MAX_PUFF_BUDGET and
// MIN/MAX_PUFFS_PER_CELL bounds to itself; this is a diagnostic readout, not a placement decision, so it is not
// this view's place to duplicate that clamp range).
SSAtmoInfoView::DeckLodData SSAtmoInfoView::deckLodData()
{
    DeckLodData d;
    if (!SSVolCloud::instanceExists())
    {
        return d;
    }
    const SSVolCloud* clouds = SSVolCloud::getInstance();
    d.mDeckBuilt = !clouds->empty();
    d.mLayerZ = d.mDeckBuilt ? clouds->cloudBaseZ()
              : (SSAtmoEnvApplier::instanceExists() ? SSAtmoEnvApplier::instance().windProfileBaseZ() : 0.f);
    d.mPuffsPlaced = clouds->puffCount();
    d.mLodPredicted = clouds->primaryLodPredictedSubs();
    d.mCellsWalked = clouds->primaryLodCellsWalked();
    d.mTierBCount = clouds->primaryTierBCount();

    static LLCachedControl<U32> budget_setting(gSavedSettings, "SSAtmoCloudPuffBudget", 2520);
    d.mBudget = (S32)budget_setting;
    static LLCachedControl<U32> dial_setting(gSavedSettings, "SSAtmoCloudPuffsPerCell", 3);
    d.mPuffsPerCell = (S32)dial_setting;
    return d;
}

// <SS:Nexii> V4's data: SSVolCloud::virgaDebug()'s snapshot copied straight across (read-only - nothing here
// re-derives qualification or the trim order), plus the ground-level wind and the active precip preset's fall
// speed the fall-tilt comparison line needs (SSAtmoInfoViewCore::fallTiltOffsetM). mKept/mTrimmed are counted
// off the snapshot's own mKept flags, never recomputed from SSVirga::keepHash - the flags already ARE that
// verdict.
SSAtmoInfoView::VirgaData SSAtmoInfoView::virgaData()
{
    VirgaData d;
    if (!SSAtmoEnvApplier::instanceExists() || !SSAtmoMagic::instanceExists() || !SSVolCloud::instanceExists())
    {
        return d;
    }

    d.mValid = SSAtmoMagic::getInstance()->hasWeather();

    const SSAtmoEnvApplier& applier = SSAtmoEnvApplier::instance();
    d.mWindGround = SSWindProfile::windAt(0.f, applier.windProfile());
    d.mFallSpeedMS = SSAtmoMagic::getInstance()->preset().mFallSpeed;

    const SSVolCloud* clouds = SSVolCloud::getInstance();
    d.mDeckBuilt = !clouds->empty();

    const SSVolCloud::SSVirgaDebug& vd = clouds->virgaDebug();
    d.mActive = vd.mActive;
    d.mGroundZ = vd.mGroundZ;
    d.mBaseZ = vd.mBaseZ;
    d.mR2 = vd.mR2;
    d.mHandoffRadius = vd.mR2 * SSVirga::HANDOFF_SKIP;
    // F9 (2026-09-06 review), 8e-b PROFILE SKEW: the emitter's own skew inputs, copied straight across - never
    // re-derived.
    d.mWindParams = vd.mWindParams;
    d.mBaseAglM = vd.mBaseAglM;
    d.mFallSpeed = vd.mFallSpeed;
    d.mEmbedTopZ = vd.mEmbedTopZ;

    d.mCells.reserve(vd.mCells.size());
    S32 kept = 0;
    for (const SSVolCloud::SSVirgaDebugCell& c : vd.mCells)
    {
        VirgaData::Cell out;
        out.mX = c.mX;
        out.mY = c.mY;
        out.mDrive = c.mDrive;
        out.mKept = c.mKept;
        if (c.mKept) ++kept;
        d.mCells.push_back(out);
    }
    d.mCandidates = (S32)vd.mCells.size();
    d.mKept = kept;
    d.mTrimmed = d.mCandidates - kept;

    return d;
}

// <SS:Nexii> V5's data: the applied track's cube sampled at CUBE_SAMPLES phases across one day cycle through the
// SAME resolvers the sky reads (SSAtmoEnvWeatherResolver::resolve for wind/shear/lightning, SSAtmoEnvCloudFieldResolver::resolve
// for gloom/anvil, SSWindProfile::consolidation, SSStormCell::gateScore for the storm-spawn curve) - every field is
// a pure function call on the track's own curves at that phase, never a respelled formula. The applied track comes
// off SSAtmoEnvApplier::appliedTrackIndex() (the SAME track the sky itself is drawn from this frame, camera-band
// selection and all), and mNowPhase off SSAtmoEnvApplier::appliedPhase() (carries the editor's preview override),
// so the cursor this view draws never disagrees with the sky beside it. The storm-spawn curve reads the anchor's
// own lattice cell (SSStormCells::anchorNow() - callable without an Interest claim, see its own comment) at the
// CURRENT epoch only (an epoch is a wall-clock bucket, not a day-cycle phase, so it is never swept per sample) with
// just the weather term varied per sample: "what would this candidate's score be if it were born at this time of
// day". [interaction: SSAtmoEnvWeatherResolver] [interaction: SSAtmoEnvCloudFieldResolver] [interaction: SSStormCell::gateScore]
SSAtmoInfoView::WeatherCubeData SSAtmoInfoView::weatherCubeData()
{
    WeatherCubeData d;
    if (!SSAtmoEnvApplier::instanceExists() || !SSAtmoEnvManager::instanceExists() || !SSAtmoMagic::instanceExists())
    {
        return d;
    }

    const SSAtmoEnvApplier& applier = SSAtmoEnvApplier::instance();
    const S32 track_idx = applier.appliedTrackIndex();
    const SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (track_idx < 0 || !mgr->hasAsset() || track_idx >= (S32)mgr->asset().mTracks.size())
    {
        return d;
    }
    if (!SSAtmoMagic::getInstance()->hasWeather())
    {
        return d;
    }

    const SSAtmoEnvTrack& track = mgr->asset().mTracks[(size_t)track_idx];
    d.mValid = true;
    d.mTrackName = track.mName;
    d.mDayLengthS = track.mDayLengthSeconds;
    d.mNowPhase = applier.appliedPhase();

    // The anchor's own lattice cell, current epoch: fixed across every sample, only the weather term is swept.
    // SSStormCells::anchorNow() is callable with no Interest claim (same idiom SSVortices' own dust-devil block
    // uses, ssvortices.cpp) and reads Vec2() (world origin) only when the agent has no region - already unlikely
    // here since hasWeather() above implies a live sky is actually applied - so this is trusted directly rather
    // than guarded on a zero-vector heuristic that would also misread a genuine anchor at the world origin.
    SSStormCell::Candidate anchor_candidate;
    d.mHaveStormScore = true;
    {
        // <SS:Nexii> Phase 8 section 2 (one clock, user 2026-09-06): the epoch is scheduler STATE (which lattice
        // candidate this is), so it runs on SSStormCells::now() (tau), never SSAtmoMagic::sharedTime() directly -
        // the same clock resolveActive's own epoch/candidate reads use, so this reconstruction's candidate id can
        // never land on an epoch the live scheduler would not have.
        const SSStormCells* sc = SSStormCells::getInstance();
        const SSStormCell::Vec2 anchor = sc->anchorNow();
        const S32 lx = (S32)std::floor(anchor.x / SSStormCell::LATTICE_M);
        const S32 ly = (S32)std::floor(anchor.y / SSStormCell::LATTICE_M);
        const S64 epoch = SSStormCell::epochOf(sc->cycleTimeNow()); // 7e D1 / 7f F4: claim-free - now() is 0 or stale when nothing drives the scheduler
        anchor_candidate = SSStormCell::candidate(SSAtmoMagic::getInstance()->seed(), lx, ly, epoch);
    }

    d.mSamples.reserve((size_t)CUBE_SAMPLES);
    for (S32 i = 0; i < CUBE_SAMPLES; ++i)
    {
        const F64 phase = (F64)cubePhaseSample(CUBE_SAMPLES, i);
        WeatherCubeData::Sample s;
        s.mPhase = (F32)phase;
        s.mMoisture = llclamp(track.mWeather.mMoisture.valueAt(phase), 0.f, 1.f);
        s.mConvection = llclamp(track.mWeather.mConvection.valueAt(phase), 0.f, 1.f);
        s.mTemperatureC = track.mWeather.mTemperatureC.valueAt(phase);

        const SSAtmoEnvWeatherState ws = SSAtmoEnvWeatherResolver::resolve(track.mWeather, phase);
        s.mWindSpeed = ws.mWindSpeed;
        s.mWindHeading = ws.mWindHeading;
        s.mShearStrength = ws.mShearStrength;
        s.mVeerDeg = ws.mVeerDeg;
        s.mLightningIntensity = llclamp(ws.mLightningIntensity, 0.f, 1.f);
        s.mLightningActive = ws.mLightningEnabled && ws.mLightningIntervalMaxSeconds > 0.f;

        s.mConsolidation = SSWindProfile::consolidation(s.mMoisture, s.mConvection);

        const SSAtmoEnvCloudFieldState fs = SSAtmoEnvCloudFieldResolver::resolve(
            track.mCloudField, track.mWeatherInfluence, s.mMoisture, s.mConvection, s.mTemperatureC, phase, track.mFloorZ);
        s.mGloom = fs.mGloom;
        s.mAnvilRamp = fs.mAnvil;

        if (d.mHaveStormScore)
        {
            SSStormCell::WeatherAtBirth w;
            w.mMoisture = s.mMoisture;
            w.mConvection = s.mConvection;
            w.mShearStrength = s.mShearStrength;
            s.mStormScore = SSStormCell::gateScore(anchor_candidate, w);
        }

        d.mSamples.push_back(s);
    }

    // <SS:Nexii> Authored override cues (doc/atmo_magic_debug_views.md V5's own marker ask): every keyframe of
    // mStormOverride/mPrecipitationOverride whose authored value is active, at THAT keyframe's own phase - the
    // instant the author placed the cue on the timeline, not SSSquall::forcedCandidate's derived cueTime (which is
    // a wall-clock instant near "now", not a fixed point on this phase axis).
    for (const auto& kf : track.mWeather.mStormOverride.keyframes())
    {
        if (kf.mValue.empty() || kf.mValue == "none") continue;
        WeatherCubeData::Cue c;
        c.mPhase = kf.mTime;
        c.mLabel = "storm: " + kf.mValue;
        c.mIsStorm = true;
        d.mCues.push_back(c);
    }
    for (const auto& kf : track.mWeather.mPrecipitationOverride.keyframes())
    {
        if (kf.mValue.empty()) continue;
        WeatherCubeData::Cue c;
        c.mPhase = kf.mTime;
        c.mLabel = "precip: " + kf.mValue;
        c.mIsStorm = false;
        d.mCues.push_back(c);
    }

    return d;
}

// <SS:Nexii> V9's data: one row per live SSStrike, scalars only (the channel itself is walked in place by
// renderLightning - see LightningData's own comment for why it is not copied), plus the applied weather's own
// lightning gate and the deferred scene lights the strike system already exports. Every read is a const getter or
// a pure core call: SSLightning::strikes()/nextStrikeIn()/sceneLights() and SSAtmoMagic's lightning row are all
// const, and the stage is SSAtmoInfoViewCore::strikeStage on fields advance() has already written this frame.
// Nothing here schedules, advances or retires a strike, and the instanceExists guards keep the view from
// constructing either singleton. [interaction: SSLightning strikes/nextStrikeIn/sceneLights] [interaction: SSAtmoMagic lightning row]
namespace
{
    // <SS:Nexii> V9: the same per-APP-FRAME memo stormCellsData() uses, for the same reason - the layer, the legend
    // and the chart each ask for this once per draw, and SSLightning::sceneLights() walks every node of every live
    // channel looking for the one nearest the camera, so an unmemoised gather paid that walk three times a frame.
    // Display-only cache of an already-computed, frame-stable read (SSLightning::idle() advances once per app
    // frame): the counter decides nothing about the world, only how often this VIEW recopies it.
    U32 sLightningDataFrame = ~0u;
    SSAtmoInfoView::LightningData sLightningDataCache;
}

SSAtmoInfoView::LightningData SSAtmoInfoView::lightningData()
{
    if (gFrameCount == sLightningDataFrame)
    {
        return sLightningDataCache;
    }

    LightningData d;
    if (!SSLightning::instanceExists())
    {
        sLightningDataFrame = gFrameCount;
        sLightningDataCache = d;
        return d;
    }

    d.mValid = true;
    d.mCloudCap = SS_MAX_STRIKE_LIGHTS;
    d.mSceneLightCap = SCENE_LIGHT_SLOTS;

    if (SSAtmoMagic::instanceExists())
    {
        const SSAtmoMagic* magic = SSAtmoMagic::getInstance();
        d.mEnabled = magic->lightningOn();
        d.mChargeOn = magic->lightningCharge();
        d.mSparksOn = magic->lightningSparks();
        d.mIntensity = magic->lightningIntensity();
        d.mIntervalMinS = magic->lightningIntervalMin();
        d.mIntervalMaxS = magic->lightningIntervalMax();
    }

    const SSLightning* lightning = SSLightning::getInstance();
    // nextStrikeIn() reads SSAtmoMagic's shared clock, so it is asked only where that singleton is already up -
    // a view must never be the thing that constructs one (the same instanceExists discipline every other data
    // gatherer here follows). Left at -1 (not scheduled) otherwise, which is exactly how the legend reads it.
    if (SSAtmoMagic::instanceExists())
    {
        d.mNextIn = lightning->nextStrikeIn();
    }

    const std::vector<SSStrike>& strikes = lightning->strikes();
    d.mStrikes.reserve(strikes.size());
    for (const SSStrike& s : strikes)
    {
        LightningData::Strike out;
        out.mKind = (S32)s.mKind;   // SSStrikeKind's values are mirrored one for one by STRIKE_KIND_* (core)
        out.mT = s.mT;
        out.mIntensity = s.mIntensity;
        out.mCharge = s.mCharge;
        out.mChargeHeld = s.mChargeHeld;
        out.mLeaderProgress = s.mLeaderProgress;
        out.mChannelBrightness = s.mChannelBrightness;
        out.mFlash = s.mFlash;
        out.mHit = s.mHit;
        out.mFire = s.mFire;
        out.mPlasmaSince = s.mPlasmaSince;
        out.mStrokeCount = s.mStrokeCount;
        out.mChannelNodes = (S32)s.mChannel.size();
        out.mCrawlCount = s.mCrawlCount;
        out.mCrawlLenM = s.mCrawlLenM;
        out.mChannelLenM = s.mChannelLenM;
        out.mDistanceM = s.mDistanceM;
        out.mSteamPeak = s.mSteamPeak;
        out.mPositive = s.mPositive;
        out.mBlue = s.mBlue;
        out.mForced = s.mForced;
        out.mAudible = s.mAudible;
        out.mOccHidden = s.mOccHidden;
        out.mOrigin = s.mOrigin;
        out.mGround = s.mGround;
        // The stage, off this strike's own clock, leader front and plasma clock: the stroke window is its own
        // rolled decay constant (a positive bolt holds its glow longer than a negative one), the plasma window
        // the shared SSDissolve::PLASMA_S the renderer's column lives for.
        out.mStage = strikeStage(s.mT, s.mLeaderProgress, s.mPlasmaSince,
                                 s.mStrokeDecayS * 2.f, SSDissolve::PLASMA_S);
        out.mLightWeight = strikeLightWeight(s.mChannelBrightness, s.mIntensity);
        // Clearing the cut is not enough to reach the shader: the uploader fills SS_MAX_STRIKE_LIGHTS slots in
        // list order and drops the rest, so a strike past the cap is marked as NOT lighting the deck - which is
        // the honest reading, and the one that explains a bright bolt the clouds ignore.
        out.mLightsCloud = strikeLightsCloud(s.mChannelBrightness, s.mIntensity) && (d.mCloudLit < d.mCloudCap);
        if (out.mLightsCloud)
        {
            ++d.mCloudLit;
        }
        d.mStrikes.push_back(out);
    }

    // The deferred point lights, from the SAME const exporter LLPipeline::renderDeferredLighting calls, asked for
    // the same slot count it asks for - so what this draws IS what the world is lit by, not a reconstruction.
    // Behind the same instanceExists guard as nextStrikeIn above: sceneLights reads the weather's own core colour.
    std::vector<LLVector4> pos_radius;
    std::vector<LLColor3> colors;
    const S32 n = SSAtmoMagic::instanceExists() ? lightning->sceneLights(pos_radius, colors, SCENE_LIGHT_SLOTS) : 0;
    d.mLights.reserve((size_t)llmax(n, 0));
    for (S32 i = 0; i < n && i < (S32)pos_radius.size() && i < (S32)colors.size(); ++i)
    {
        LightningData::SceneLight light;
        light.mPos.setVec(pos_radius[i].mV[0], pos_radius[i].mV[1], pos_radius[i].mV[2]);
        light.mRadiusM = pos_radius[i].mV[3];
        light.mColor = colors[i];
        d.mLights.push_back(light);
    }

    sLightningDataFrame = gFrameCount;
    sLightningDataCache = d;
    return d;
}

// <SS:Nexii> V10's data: the world field's baked acoustic channel, read straight off
// the camera region's tile through SSWorldField::acousticDebug (probes and links in
// range, the listener's blend set), plus the soundscape's last thunder with its
// propagation figures - the pair the debug lines draw (direct vs propagated path).
// Read-only over state the systems already hold; mValid mirrors the bake's own
// serial gate, so a stale channel reads "stale" rather than drawing yesterday's room.
SSAtmoInfoView::AcousticsData SSAtmoInfoView::acousticsData()
{
    AcousticsData d;
    static LLCachedControl<bool> enabled(gSavedSettings, "SSWorldFieldAcoustics", true);
    static LLCachedControl<U32> quality(gSavedSettings, "SSWorldFieldAcousticsQuality", 0);
    d.mEnabled = (bool)enabled;
    d.mQuality = (U32)quality;
    if (!d.mEnabled) return d;
    if (!SSWorldField::instanceExists() || !SSAtmoMagic::instanceExists()) return d;
    if (!SSAtmoMagic::getInstance()->hasWeather()) return d;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(
        LLViewerCamera::getInstance()->getOrigin());
    if (!regionp) return d;

    // The debug range: where the probe lattice stops being readable, and past the
    // lattice's own ~8 m spacing by a wide factor so context survives the cull.
    static LLCachedControl<F32> range_setting(gSavedSettings, "SSAtmoWindFlowDebugRange", 24.f);
    const F32 range = llclamp((F32)range_setting, 16.f, 4096.f);

    if (!SSWorldField::getInstance()->acousticDebug(regionp->getHandle(),
                                                    LLViewerCamera::getInstance()->getOrigin(),
                                                    range * 2.f, d.mField))
    {
        return d;
    }

    d.mValid = true;
    d.mProbeCount = d.mField.mProbeCount;
    d.mBundleCount = d.mField.mBundleCount;
    d.mLinkCount = d.mField.mLinkCount;
    d.mPortalCount = d.mField.mPortalCount;
    d.mLatCell = d.mField.mLatCell;

    if (SSSoundscape::instanceExists())
    {
        d.mThunder = SSSoundscape::getInstance()->lastThunderPath();
        if (d.mThunder.mValid)
        {
            d.mThunderAge = SSAtmoMagic::getInstance()->sharedTime() - d.mThunder.mWhen;
        }
    }
    return d;
}

// ---------------------------------------------------------------------------
// The in-world layer: V1's wind mast
// ---------------------------------------------------------------------------

// <SS:Nexii> mat3(modelview) * w, renormalised, into three floats the shader can take as a vec3. A w of 0 makes it a DIRECTION transform - no translation - which is what turns a world-space direction into the view space the G-buffer normals live in, and renormalising costs nothing while covering the case where whatever loaded the modelview did not hand us a rigid transform. [interaction: SSAtmoInfoView::renderInfoLook]
static void ssLookViewDir(const glm::mat4& mv, F32 wx, F32 wy, F32 wz, F32 out[3])
{
    const glm::vec4 v = mv * glm::vec4(wx, wy, wz, 0.f);
    F32 x = v.x;
    F32 y = v.y;
    F32 z = v.z;
    const F32 len = sqrtf(x * x + y * y + z * z);
    if (len > 1.0e-6f)
    {
        x /= len;
        y /= len;
        z /= len;
    }
    else
    {
        x = 0.f;
        y = 0.f;
        z = 1.f;
    }
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

// <SS:Nexii> The LOOK pass: one full-screen triangle that REPLACES the world with a warm-gray reading of itself (doc/atmo_magic_phase8_show.md section 6 item 4). It runs over the finished, presented frame, so it is not a tint over a tint - blend is OFF and it overwrites every pixel of the world viewport. Three inputs: the PRESENTED colour, which is gPipeline.mSSLastPresented (the exact target renderFinalize handed the "Present the screen target" pass as DEFERRED_DIFFUSE - pipeline.cpp records it on the line above that bind, because the post chain's ping-pong means no fixed target name is right on every path), plus the G-buffer's depth and its normal attachment (mRT->deferredScreen attachment 2), which the post chain never touches. It deliberately does NOT go through gPipeline.bindDeferredShader: that binds deferredScreen's attachment 0 as diffuseRect, i.e. the raw albedo, which is exactly the buffer this pass must not read - the whole point is that everything the eye was shown (tonemap, exposure, CAS, glow, DoF, FSAA, the vignette) is already in the colour. Nor does it read the pipeline's SSAO: mRT->deferredLight is overwritten by the post chain long before this point, so the shader recomputes a 12-tap hemisphere occlusion from depth and normal itself. Alpha-blended surfaces need no special case and get none: water, glass, particles and the rain curtains are already composited into the presented colour, and they take the occlusion, shade and fog of the OPAQUE surface behind them, because depth and normal at that pixel are the opaque one's - a real fog does the same to a pane of glass, and that equivalence is why the design chose a post-screen pass over a warm-gray variant of every material shader in the fork. The two direction uniforms are the shader's entire light model, both in VIEW space because that is the space the G-buffer normals live in: ss_look_up is world up through mat3(modelview), ss_look_key is the environment's light direction flattened to its azimuth and lifted back to SSInfoLook::LOOK_KEY_ELEVATION, so faces read apart without the key ever going flat at noon or vanishing at night. Depth test and blend are both off and the viewport is the one setup3DRender just set, which is the same mWorldViewRectRaw renderFinalize presented into - so the pass covers exactly the world and leaves the UI margins outside it alone. [interaction: LLPipeline::mSSLastPresented] [interaction: ssInfoLookF.glsl]
void SSAtmoInfoView::renderInfoLook()
{
    static LLCachedControl<bool> look_setting(gSavedSettings, "SSAtmoInfoViewLook", true);
    if (!(bool)look_setting) return;

    if (!gSSInfoLookProgram.isComplete()) return;
    if (!gPipeline.mRT) return;
    if (gPipeline.mScreenTriangleVB.isNull()) return;

    LLRenderTarget* presented = gPipeline.mSSLastPresented;
    LLRenderTarget* gbuf = &gPipeline.mRT->deferredScreen;
    if (!presented || presented->getWidth() == 0) return;
    if (gbuf->getWidth() == 0) return;

    LL_PROFILE_GPU_ZONE("atmo info look");

    gSSInfoLookProgram.bind();

    // The presented colour goes on diffuseRect (DEFERRED_DIFFUSE) - this shader's own declaration of that name, not the G-buffer's.
    S32 channel = gSSInfoLookProgram.enableTexture(LLShaderMgr::DEFERRED_DIFFUSE, presented->getUsage());
    if (channel > -1)
    {
        presented->bindTexture(0, channel, LLTexUnit::TFO_POINT);
        gGL.getTexUnit(channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
    }

    // Attachment 2 of deferredScreen is frag_data[2]: the packed normal, plus the gbuffer flag in w that the sky test reads.
    channel = gSSInfoLookProgram.enableTexture(LLShaderMgr::NORMAL_MAP, gbuf->getUsage());
    if (channel > -1)
    {
        gbuf->bindTexture(2, channel, LLTexUnit::TFO_POINT);
        gGL.getTexUnit(channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
    }

    channel = gSSInfoLookProgram.enableTexture(LLShaderMgr::DEFERRED_DEPTH, gbuf->getUsage());
    if (channel > -1)
    {
        gGL.getTexUnit(channel)->bind(gbuf, true);
    }

    // The two directions, built in world space and carried into view space by the modelview setup3DRender just loaded.
    const glm::mat4 mv = get_current_modelview();

    LLVector3 key_world = LLEnvironment::instance().getLightDirection();
    key_world.mV[VZ] = 0.f;
    if (key_world.magVecSquared() < 1.0e-6f)
    {
        key_world.setVec(1.f, 0.f, 0.f);
    }
    key_world.normVec();
    key_world.mV[VZ] = SSInfoLook::LOOK_KEY_ELEVATION;
    key_world.normVec();

    F32 up_v[3];
    F32 key_v[3];
    ssLookViewDir(mv, 0.f, 0.f, 1.f, up_v);
    ssLookViewDir(mv, key_world.mV[VX], key_world.mV[VY], key_world.mV[VZ], key_v);

    static LLStaticHashedString s_look_up("ss_look_up");
    static LLStaticHashedString s_look_key("ss_look_key");
    gSSInfoLookProgram.uniform3fv(s_look_up, 1, up_v);
    gSSInfoLookProgram.uniform3fv(s_look_key, 1, key_v);

    {
        LLGLDisable cull(GL_CULL_FACE);
        LLGLDisable blend(GL_BLEND);
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
    }

    // Release every unit this pass claimed: the world layer below draws with gUIProgram on unit 0, and leaving a
    // render target's colour texture resident on a unit is how the whiteout pass earned its feedback-loop comment.
    gSSInfoLookProgram.disableTexture(LLShaderMgr::DEFERRED_DIFFUSE, presented->getUsage());
    gSSInfoLookProgram.disableTexture(LLShaderMgr::NORMAL_MAP, gbuf->getUsage());
    gSSInfoLookProgram.disableTexture(LLShaderMgr::DEFERRED_DEPTH, gbuf->getUsage());
    gSSInfoLookProgram.unbind();

    gGL.setSceneBlendType(LLRender::BT_ALPHA);
}

// <SS:Nexii> The overlay's ONE 3-D draw site, called from render_ui() in llviewerdisplay.cpp AFTER gPipeline.renderFinalize() and BEFORE render_hud_attachments(): post-tonemap, a 3-D pass with the WORLD camera, in the same window between finalize and the HUD where HUD attachments draw their own 3-D geometry. It draws the LOOK pass first, then the in-world layer on top of it (each render* helper keeps depth test off), which is the Skylines look - the world becomes a warm-gray model, the data stays bright on top of it. What used to be here was a translucent near-black quad driven by SSAtmoInfoViewDim; the user's verdict was "the world dimming still sucks, it is just making the world black at max", so the quad and its slider are gone and renderInfoLook() has their place - see that function for what it reads and why. Two earlier homes were wrong for two different reasons: SSAtmoDimView::draw() in the UI stage (a 3-D layer re-entering from the UI pass clipped the console text and threw the visualisations into the window corners), then LLPipeline::renderDebug, which runs inside renderGeomPostDeferred and so landed the overlay in the HDR screen buffer BEFORE generateLuminance/tonemap - a 0.55 linear tint read as roughly 0.30 perceptual AND auto-exposure then opened up to cancel it over about a second, pumping the whole scene. Drawn here everything lands in the default framebuffer in display space and nothing feeds back into exposure; the look pass needs that just as much, because its ramp is authored against display-space luminance. Nothing 2-D is left in this function - the dim quad was the only thing that wanted setup2DRender's raw-window ortho (and needed it, or a UI scale above 1 left an undimmed strip); the look pass is a clip-space triangle over the world viewport instead, which is the rect the world was actually presented into. Matrix discipline is render_hud_attachments' own, not the LLSceneMonitor block's: push BOTH stacks and save the cached copies, then setup3DRender() (it reloads projection AND modelview from LLViewerCamera and resets the viewport to mWorldViewRectRaw - the same viewport renderFinalize left set), which is what gives the look pass its projection_matrix/inv_proj and its view-space directions as well as giving the world layer its camera, then setup3DRender()/pop/restore on the way out so viewport, GL matrices and the get_current_* cache all come back exactly as found; gGLLastMatrix is cleared because those loads went in behind the pipeline's cache. render_hud_attachments re-derives its own matrices from setup_hud_matrices and restores what it saw, and render_hud_elements (which runs first, in world space) needs precisely the state we restore. gUIProgram (declared by llrender2dutils.h, already included here for gl_rect_2d) is bound around the world layer, which needs a shader as much as any pass does, and unbound at the end, because renderFinalize leaves no shader bound and render_hud_elements binds its own. Runs with the look off: SSAtmoInfoViewLook FALSE means "no look", not "no overlay". [interaction: render_ui] [interaction: render_hud_attachments] [interaction: SSAtmoDimView]
void SSAtmoInfoView::renderDimAndWorld()
{
    // <SS:Nexii> V9's mask (RENDER_DEBUG_LIGHTNING, the eighth switch on the debug floater beside the other
    // seven): the lightning layer is BOTH an info view and an engineering overlay, so this function also runs
    // with no mode selected at all when that mask is set. The LOOK pass stays tied to the MODE - a checkbox
    // overlay must not repaint the world warm-gray - so with the mask on and the mode off, the world is left
    // exactly as rendered and only the layer draws. This is why the mask is read here rather than dispatched from
    // LLPipeline::renderDebug like the other seven: renderDebug runs inside renderGeomPostDeferred, ahead of the
    // luminance sample, which is the very reason this whole overlay was moved out of it (see the comment on
    // renderWorld's caller in pipeline.cpp). [interaction: LLPipeline::RENDER_DEBUG_LIGHTNING]
    const U32 active_mode = mode();
    const bool lightning_mask = gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_LIGHTNING);
    if (active_mode == MODE_OFF && !lightning_mask) return;
    if (!gViewerWindow) return;

    gGL.flush();

    // <SS:Nexii> Save everything setup3DRender is about to overwrite: both GL matrix stacks and the get_current_* cache the UI stage and the HUD passes read back out of.
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    const glm::mat4 saved_projection = get_current_projection();
    const glm::mat4 saved_modelview = get_current_modelview();

    gViewerWindow->setup3DRender();

    if (active_mode != MODE_OFF)
    {
        renderInfoLook();
    }

    gUIProgram.bind();

    {
        LLGLDisable cull(GL_CULL_FACE);
        LLGLEnable blend(GL_BLEND);
        gGL.setSceneBlendType(LLRender::BT_ALPHA);

        renderWorld();

        gGL.flush();
    }

    // <SS:Nexii> setup3DRender() first for the viewport and the matrices, then the stacks and the cache, so what follows sees exactly the state renderFinalize left.
    gViewerWindow->setup3DRender();
    gUIProgram.unbind();
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
    set_current_projection(saved_projection);
    set_current_modelview(saved_modelview);
    gGLLastMatrix = NULL;
}

// Dispatch on the live mode; each layer guards its own data. V9's layer is the one exception to "one mode, one
// layer": it also answers to RENDER_DEBUG_LIGHTNING, so it is drawn EXACTLY ONCE whether the mode selects it, the
// mask does, or both. The reserved modes (V6 World Field, V8 Anatomy - see SSAtmoInfoViewCore's numbering) have no
// case here on purpose; they fall through and draw nothing until someone builds them.
void SSAtmoInfoView::renderWorld()
{
    const U32 active_mode = mode();
    const bool lightning = (active_mode == MODE_LIGHTNING) ||
                           gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_LIGHTNING);
    if (lightning)
    {
        renderLightning();
    }

    switch (active_mode)
    {
        case MODE_WIND_PROFILE: renderWindMast(); break;
        case MODE_STORM_CELLS:  renderStormCells(); break;
        case MODE_DECK_LOD:     renderDeckLod(); break;
        case MODE_PRECIP_VIRGA: renderVirga(); break;
        case MODE_ACOUSTICS:    renderAcoustics(); break;
        default: break;
    }
}

// A vertical stack of arrows from the track floor to the cirrus band, each rotated and scaled by windAt(z) and coloured by the speed ramp, so shear reads as the stack twisting with height. Stood a little ahead of the camera (SSAtmoInfoViewMastOffset, 0 for the camera column itself) and pulled through the cloud field's far squash so the upper rungs land beside the deck they describe. [interaction: SSVolCloud squashScale]
void SSAtmoInfoView::renderWindMast()
{
    const WindProfileData d = windProfileData();
    if (!d.mValid) return;

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    if (!camera) return;

    static LLCachedControl<F32> offset_setting(gSavedSettings, "SSAtmoInfoViewMastOffset", 80.f);

    const LLVector3 cam = camera->getOrigin();
    LLVector3 forward = camera->getAtAxis();
    forward.mV[VZ] = 0.f;
    if (forward.magVecSquared() < 1.0e-6f)
    {
        forward.setVec(1.f, 0.f, 0.f);
    }
    forward.normVec();

    const F32 offset = llclamp((F32)offset_setting, 0.f, 2000.f);
    const F32 mx = cam.mV[VX] + forward.mV[VX] * offset;
    const F32 my = cam.mV[VY] + forward.mV[VY] * offset;
    const F32 ground = d.mGroundZ;
    const F32 top_agl = d.mTopAgl;

    const SSVolCloud* clouds = SSVolCloud::instanceExists() ? SSVolCloud::getInstance() : nullptr;
    auto drawn = [&](const LLVector3& p) -> LLVector3
    {
        if (!clouds) return p;
        const LLVector3 rel = p - cam;
        const F32 dist = rel.magVec();
        if (dist <= 1.0e-4f) return p;
        return cam + rel * clouds->squashScale(dist);
    };

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_FALSE, GL_FALSE); // <SS:Nexii> off, not just no-write: this draws in the 3-D pass AFTER SSAtmoInfoView::renderDimAndWorld has laid the dim quad down, on top like a Skylines layer, so it must never be occluded by world geometry
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    auto line = [&](const LLVector3& a, const LLVector3& b)
    {
        const LLVector3 span = b - a;
        const S32 segs = llclamp((S32)(span.magVec() / 200.f), 1, 24);
        LLVector3 prev = drawn(a);
        for (S32 i = 1; i <= segs; ++i)
        {
            const LLVector3 next = drawn(a + span * ((F32)i / (F32)segs));
            gGL.vertex3fv(prev.mV);
            gGL.vertex3fv(next.mV);
            prev = next;
        }
    };

    gGL.begin(LLRender::LINES);

    // The pole.
    gGL.color4f(0.8f, 0.8f, 0.8f, 0.35f);
    line(LLVector3(mx, my, ground), LLVector3(mx, my, ground + top_agl));

    // The rungs.
    const S32 n = mastRungCount(top_agl);
    for (S32 i = 0; i < n; ++i)
    {
        const F32 agl = mastRungAgl(i, top_agl);
        const F32 below = (i > 0) ? mastRungAgl(i - 1, top_agl) : 0.f;
        const F32 above = (i + 1 < n) ? mastRungAgl(i + 1, top_agl) : top_agl + (agl - below);
        const F32 gap = llmax(llmin(agl - below, above - agl), 10.f);

        const SSWindProfile::Vec2 w = SSWindProfile::windAt(agl, d.mParams);
        const F32 speed = speedOf(w);
        LLVector3 dir(0.f, 1.f, 0.f);
        if (speed > 1.0e-4f)
        {
            dir.setVec(w.x / speed, w.y / speed, 0.f);
        }
        const F32 len = llmax(mastArrowLen(speed, d.mMaxSpeed, gap), 4.f);
        const LLVector3 centre(mx, my, ground + agl);
        const LLVector3 tip = centre + dir * len;
        const LLVector3 perp(-dir.mV[VY], dir.mV[VX], 0.f);
        const LLVector3 barb_a = tip - dir * (len * 0.22f) + perp * (len * 0.12f);
        const LLVector3 barb_b = tip - dir * (len * 0.22f) - perp * (len * 0.12f);

        const LLColor4 c = toColor(speedRamp(speed, d.mMaxSpeed), 0.95f);
        gGL.color4f(c.mV[0] * 0.25f, c.mV[1] * 0.25f, c.mV[2] * 0.25f, 0.6f);
        gGL.vertex3fv(drawn(centre).mV);
        gGL.color4fv(c.mV);
        gGL.vertex3fv(drawn(tip).mV);
        line(tip, barb_a);
        line(tip, barb_b);
    }

    // The rails: a cross on the pole at each load-bearing altitude, in the rail's own colour.
    std::vector<Rail> rails;
    collectRails(d, rails);
    for (const Rail& rail : rails)
    {
        if (!rail.mPresent || rail.mAgl < 0.f || rail.mAgl > top_agl) continue;
        const F32 tick = llmax(20.f, rail.mAgl * 0.04f);
        const F32 z = ground + rail.mAgl;
        gGL.color4fv(rail.mColor->mV);
        line(LLVector3(mx - tick, my, z), LLVector3(mx + tick, my, z));
        line(LLVector3(mx, my - tick, z), LLVector3(mx, my + tick, z));
    }

    gGL.end();

    // Labels: every rung's altitude, speed and heading; every rail its name.
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    if (font)
    {
        for (S32 i = 0; i < n; ++i)
        {
            const F32 agl = mastRungAgl(i, top_agl);
            const SSWindProfile::Vec2 w = SSWindProfile::windAt(agl, d.mParams);
            const F32 speed = speedOf(w);
            const std::string label = llformat("%.0f m  %.1f m/s  %03.0f", agl, speed, headingOfVec(w.x, w.y));
            const LLColor4 c = toColor(speedRamp(speed, d.mMaxSpeed), 1.f);
            hud_render_utf8text(label, drawn(LLVector3(mx, my, ground + agl)), *font, LLFontGL::NORMAL,
                                LLFontGL::DROP_SHADOW, 6.f, -4.f, c, false);
        }
        for (const Rail& rail : rails)
        {
            if (!rail.mPresent || rail.mAgl < 0.f || rail.mAgl > top_agl) continue;
            hud_render_utf8text(rail.mLabel, drawn(LLVector3(mx, my, ground + rail.mAgl)), *font, LLFontGL::NORMAL,
                                LLFontGL::DROP_SHADOW, -6.f - (F32)font->getWidth(rail.mLabel), 4.f, *rail.mColor, false);
        }
    }
}

// ---------------------------------------------------------------------------
// The in-world layer: V2's storm cells
// ---------------------------------------------------------------------------

namespace
{
    // <SS:Nexii> S12 (phase-2b audit): stormCellsData() re-walks the lattice and both cell lists every call; V2's
    // in-world layer, its legend and its chart panel each called it once per draw, so a single frame with V2 active
    // paid the walk three times over. Memoised per APP FRAME (gFrameCount) rather than per wall-clock tick: this is
    // a display-only cache of an already-computed, frame-stable read (SSStormCells::update() ticks once per app
    // frame, and gAgent's position - which toAgentXY reads - does not move mid-frame), never a determinism input,
    // so a frame counter here decides nothing about the world, only how often this VIEW recomputes its own copy of it.
    U32 sStormCellsDataFrame = ~0u;
    SSAtmoInfoView::StormCellsData sStormCellsDataCache;
}

// Every read is a const getter on the scheduler's frame (cells/hero/whyNot as update() left them) or a pure core call on published inputs; nothing here can spawn, steer or reorder a cell. The lattice is re-read through SSStormCell::enumerateLattice / candidate at the CURRENT epoch for the tile tints - the same pure functions the scheduler itself walks.
SSAtmoInfoView::StormCellsData SSAtmoInfoView::stormCellsData()
{
    if (gFrameCount == sStormCellsDataFrame)
    {
        return sStormCellsDataCache;
    }

    using namespace SSStormCell;
    StormCellsData d;
    if (!SSStormCells::instanceExists())
    {
        sStormCellsDataFrame = gFrameCount;
        sStormCellsDataCache = d;
        return d;
    }
    const SSStormCells* sc = SSStormCells::getInstance();
    if (!sc->valid())
    {
        sStormCellsDataFrame = gFrameCount;
        sStormCellsDataCache = d;
        return d;
    }

    d.mValid = true;
    d.mNow = sc->now();
    d.mAnchorAgent = sc->toAgentXY(sc->anchor());

    const SSStormCells::WhyNot& why = sc->whyNot();
    d.mCandidates = why.mCandidates;
    d.mAlive = why.mAlive;
    d.mSpawned = why.mSpawned;
    d.mSupercells = why.mSupercells;
    d.mTornadoEligible = why.mTornadoEligible;
    d.mWhyNot = why.mFailing;

    // The applied track's live checkboxes (the gate reads them at each birth; the legend wants the current state).
    if (SSAtmoEnvManager::instanceExists())
    {
        const SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
        const S32 track = sc->trackIndex();
        if (mgr->hasAsset() && track >= 0 && track < (S32)mgr->asset().mTracks.size())
        {
            const SSAtmoEnvWeatherInfluence& infl = mgr->asset().mTracks[(size_t)track].mWeatherInfluence;
            d.mAllowSupercells = infl.mEnabled && infl.mAllowSupercells;
            d.mAllowTornadoes = infl.mEnabled && infl.mAllowTornadoes;
        }
    }

    // Altitude: the deck base the cells will one day modulate, else the drift base the applier integrates at.
    if (SSVolCloud::instanceExists() && !SSVolCloud::getInstance()->empty())
    {
        d.mDeckBuilt = true;
        d.mLayerZ = SSVolCloud::getInstance()->cloudBaseZ();
    }
    else if (SSAtmoEnvApplier::instanceExists())
    {
        d.mLayerZ = SSAtmoEnvApplier::instance().windProfileBaseZ();
    }

    // Active cells, in the scheduler's id order.
    const std::vector<SSStormCells::ActiveCell>& cells = sc->cells();
    d.mCells.reserve(cells.size());
    for (const SSStormCells::ActiveCell& c : cells)
    {
        StormCellsData::Cell out;
        out.mId = c.mCandidate.mId; // S6: ActiveCell is now directly the core POD - mCandidate.mId is the ranking key, not a top-level field
        out.mCentreAgent = sc->toAgentXY(c.mCentre);
        out.mRadiusM = c.mRadiusM;
        out.mStage = (S32)c.mLifecycle.mStage;
        out.mAge01 = c.mAge01;
        out.mBirthTime = c.mCandidate.mBirthTime;
        out.mLifetimeS = c.mCandidate.mLifetimeS;
        out.mRotation = c.mCandidate.mRotation;
        out.mMeso = c.mLifecycle.mMeso;
        out.mIntensity = c.mGate.mIntensity;
        out.mSupercell = c.mGate.mSupercell;
        out.mIsHero = c.mIsHero;
        out.mLineId = c.mLineId;
        out.mIsForced = c.mIsForced;
        d.mCells.push_back(out);
    }

    // The hero's full trajectory.
    if (const SSStormCells::Hero* hero = sc->hero())
    {
        d.mHaveHero = true;
        d.mHero.mId = hero->mId;
        d.mHero.mOriginAgent = sc->toAgentXY(hero->mPath.mOrigin);
        d.mHero.mNowAgent = sc->toAgentXY(centreAt(hero->mPath.mOrigin, hero->mPath.mMotion, hero->mBirthTime, d.mNow));
        d.mHero.mDeathAgent = sc->toAgentXY(hero->mDeath);
        d.mHero.mClosestAgent = sc->toAgentXY(hero->mPath.mClosest);
        d.mHero.mMotion.setVec(hero->mPath.mMotion.x, hero->mPath.mMotion.y);
        d.mHero.mClosestDistM = hero->mPath.mClosestDistM;
        d.mHero.mBirthTime = hero->mBirthTime;
        d.mHero.mClosestTime = hero->mPath.mClosestTime;
        d.mHero.mLifetimeS = hero->mLifetimeS;
        for (const SSStormCells::ActiveCell& c : cells)
        {
            if (c.mIsHero) { d.mHero.mRotation = c.mCandidate.mRotation; break; }
        }
    }

    // <SS:Nexii> SQUALL (doc/atmo_magic_storm_dynamics.md section 5, V2's own "bar through the members" ask):
    // reconstructs each active line's geometry read-only, through the SAME pure functions the scheduler used to
    // spawn it (SSSquall::lineMember/qlcsJunctionAt), never a new field carried on ActiveCell beyond the mLineId it
    // already has. 8a audit F3: the LineEvent itself is no longer RE-DERIVED here (the old version called
    // SSSquall::lineEvent unconditionally, which only ever reproduces the HASHED branch - an authored line's
    // mLineId is chained through forcedLine's own SALT_FORCED_LINE_ID salt, so lineEvent() here could never
    // recompute a matching id for it and an authored squall silently drew no bar/junctions at all). Instead this
    // reads sc->lineEventForId(lineId), the EXACT event SSStormCells::update()'s lineAtEpoch resolved this frame
    // (hashed or authored) and stored alongside its LineDesc - so the reconstruction is honest for both. Members
    // are matched back to active cells by mCandidate.mId (the SAME pure function applied to the SAME (lineId, i)
    // gives the SAME id, sssquallcore.h's lineMember), so mMembersAgent only ever contains members that actually
    // gated and are alive THIS frame, in the template's own along-the-line order (the offset formula is monotone
    // in member index). 7b F9: qlcsJunctionAt advects the birth-frame junction to the wall time the cells were
    // resolved at (sc->now()) - qlcsJunction alone is the birth-frame position, and drawing it beside members
    // already advected by mMotion * age would separate the marker from the line by |motion| * age (lesson 12).
    {
        // Distinct line ids among this frame's active cells.
        std::vector<U64> line_ids;
        for (const SSStormCells::ActiveCell& c : cells)
        {
            if (c.mLineId == 0) continue;
            bool have = false;
            for (U64 id : line_ids) { if (id == c.mLineId) { have = true; break; } }
            if (!have) line_ids.push_back(c.mLineId);
        }

        for (U64 lineId : line_ids)
        {
            const SSSquall::LineEvent* e = sc->lineEventForId(lineId);
            if (!e || !e->mIsLine || e->mLineId != lineId) continue; // defensive: should be unreachable

            StormCellsData::SquallLine sl;
            sl.mLineId = lineId;
            const S32 n = llmin((S32)SSSquall::LINE_MEMBERS_MAX, SSStormCell::LINE_MEMBERS_CAP);
            for (S32 i = 0; i < n; ++i)
            {
                const SSStormCell::Candidate m = SSSquall::lineMember(*e, i);
                for (const SSStormCells::ActiveCell& c : cells)
                {
                    if (c.mLineId == lineId && c.mCandidate.mId == m.mId)
                    {
                        sl.mMembersAgent.push_back(sc->toAgentXY(c.mCentre));
                        break;
                    }
                }
            }
            for (S32 i = 0; i + 1 < n; ++i)
            {
                const SSSquall::Vec2 j = SSSquall::qlcsJunctionAt(*e, i, sc->now());
                sl.mJunctionsAgent.push_back(sc->toAgentXY(SSStormCell::Vec2{ j.x, j.y }));
            }
            if (!sl.mMembersAgent.empty())
            {
                d.mSquallLines.push_back(sl);
            }
        }
    }

    // Lattice tiles within the field, tinted by this epoch's potential.
    constexpr S32 CAP = 256;
    S32 lx[CAP];
    S32 ly[CAP];
    const S32 count = llmin(enumerateLattice(sc->anchor(), SSStormCells::FIELD_M, lx, ly, CAP), CAP);
    d.mTiles.reserve((size_t)count);
    for (S32 i = 0; i < count; ++i)
    {
        const Candidate c = candidate(sc->seed(), lx[i], ly[i], sc->currentEpoch());
        StormCellsData::Tile t;
        Vec2 centre;
        centre.x = ((F32)lx[i] + 0.5f) * LATTICE_M;
        centre.y = ((F32)ly[i] + 0.5f) * LATTICE_M;
        t.mCentreAgent = sc->toAgentXY(centre);
        t.mPotential = c.mPotential;
        t.mShearNoise = c.mShearNoise;
        t.mLX = lx[i];
        t.mLY = ly[i];
        // <SS:Nexii> S10 (phase-2b audit): match by the alive cell's ACTUAL origin tile, not its natal
        // candidate.mLX/mLY - for the hero those differ (composeHero overrides mOrigin up to HERO_SPAWN_MAX_M from
        // the anchor, which can land in a different lattice cell than the one it was born on), so this is the tile
        // the hero's marker actually follows. Every non-hero cell's mOrigin floors to the same cell as its
        // candidate's natal mLX/mLY (jitter never crosses a cell boundary - JITTER_FRAC <= 0.35 keeps it inside
        // [0.15, 0.85] of the cell), so this is a no-op for them. When alive, the tile is tinted by THAT cell's own
        // mCandidate.mPotential (its value at ITS OWN birth epoch), not this loop's freshly recomputed candidate()
        // at the CURRENT epoch, which can legitimately differ - POTENTIAL_DRIFT scrolls the field per epoch, so an
        // older survivor's birth-epoch potential is not what "now"'s epoch would draw for that same lattice cell.
        for (const SSStormCells::ActiveCell& a : cells)
        {
            const S32 ax = (S32)std::floor(a.mOrigin.x / LATTICE_M);
            const S32 ay = (S32)std::floor(a.mOrigin.y / LATTICE_M);
            if (ax == lx[i] && ay == ly[i])
            {
                t.mAlive = true;
                t.mPotential = a.mCandidate.mPotential;
                break;
            }
        }
        d.mTiles.push_back(t);
    }

    // <SS:Nexii> V2 LINE BAND (doc/atmo_magic_phase8_show.md section 3 item 1): the deck coupling's own closed-form
    // band, read straight off SSStormCells::fillLineBand (already AGENT frame - see its own comment) rather than
    // reconstructed - mStrength stays 0 (LineBand's own "disabled" reading) when no line is alive this frame.
    {
        SSStormCouple::LineBand lb;
        sc->fillLineBand(lb);
        d.mLineBand.mOriginAgent.setVec(lb.ox, lb.oy);
        d.mLineBand.mDir.setVec(lb.dirX, lb.dirY);
        d.mLineBand.mMotion.setVec(lb.motX, lb.motY);
        d.mLineBand.mHalfLenM = lb.halfLen;
        d.mLineBand.mBandM = lb.bandM;
        d.mLineBand.mShelfM = lb.shelfM;
        d.mLineBand.mStrength = lb.strength;
    }

    // <SS:Nexii> DEBUG: vortex icons - SSVortices::update() is ticked unconditionally from SSAtmoMagic::idle()
    // right after SSStormCells::update(), so this is the SAME frame's resolved state as everything gathered above
    // it. SCHEDULER fix 9, revised by review NEW-3: SSVortices claims its OWN SSStormCells::Interest only while the
    // applied track's mAllowSupercells/mAllowTornadoes could actually produce a live storm-cell child this frame
    // (see SSVortices::update()'s own comment in the .cpp for the cost-gate reasoning) - this view claims nothing
    // extra either way, and dustDevils()/active() below may both be empty on a frame where that gate is off and
    // nothing else holds SSStormCells' Interest. Read-only: every field below is copied straight off
    // active()/dustDevils()/whyNot() as the scheduler left them. [interaction: SSVortices]
    if (SSVortices::instanceExists() && SSVortices::getInstance()->valid())
    {
        const SSVortices* vortices = SSVortices::getInstance();

        d.mVortices.reserve(vortices->active().size());
        for (const SSVortices::LiveVortex& v : vortices->active())
        {
            StormCellsData::VortexIcon icon;
            icon.mId = v.mCandidate.mId;
            icon.mKind = (S32)v.mKind;
            icon.mRotationSign = v.mCandidate.mRotSign;
            icon.mParentIsHero = v.mParentIsHero;
            icon.mWasRelabelledWaterspout = v.mWasRelabelledWaterspout;
            icon.mContactAgent = sc->toAgentXY(toStormVec2(v.mContactGlobal));
            icon.mCondensation = v.mState.mCondensation;
            icon.mIntensity = v.mState.mIntensity;
            icon.mMultiN = v.mCandidate.mMultiN;
            icon.mHasFunnel = v.mHasFunnel;
            if (v.mHasFunnel)
            {
                icon.mCollars.reserve(v.mCollars.size());
                for (const SSVortices::Collar& c : v.mCollars)
                {
                    StormCellsData::VortexIcon::Collar out;
                    out.mRadiusM = c.mRadiusM;
                    out.mAltitudeZ = c.mAltitudeM;
                    icon.mCollars.push_back(out);
                }
            }
            d.mVortices.push_back(icon);
        }

        d.mDustDevils.reserve(vortices->dustDevils().size());
        for (const SSVortices::DustVortex& dv : vortices->dustDevils())
        {
            StormCellsData::DustIcon di;
            di.mId = dv.mCandidate.mId;
            di.mOriginAgent = sc->toAgentXY(toStormVec2(dv.mCandidate.mOriginXY));
            di.mIntensity = dv.mIntensity;
            d.mDustDevils.push_back(di);
        }

        const SSVortices::WhyNotTornado& vwhy = vortices->whyNot();
        d.mVortexWhyNot.mHaveHero = vwhy.mHaveHero;
        d.mVortexWhyNot.mKind = (S32)vwhy.mKind;
        d.mVortexWhyNot.mAlive = vwhy.mAlive;
        d.mVortexWhyNot.mMeso = vwhy.mMeso;
        d.mVortexWhyNot.mPotential = vwhy.mPotential;
        d.mVortexWhyNot.mSupercell = vwhy.mSupercell;
        d.mVortexWhyNot.mTornadoEligible = vwhy.mTornadoEligible;
        d.mVortexWhyNot.mAllowTornadoes = vwhy.mAllowTornadoes;
        d.mVortexWhyNot.mFailing = vwhy.mFailing;
    }

    sStormCellsDataFrame = gFrameCount;
    sStormCellsDataCache = d;
    return d;
}

// The V2 layer (doc/atmo_magic_debug_views.md V2), drawn at the deck base: lattice tiles tinted by potential (energy ramp, alpha floored at the spawn threshold), alive cells as influence rings coloured by stage with the 40% plateau ring inside, the hero's trajectory ribbon origin -> now -> death coloured by lifecycle with age ticks and the closest-approach marker tied to the anchor, rotation as orbiting purple arrows (anticlockwise for cyclonic, clockwise for anticyclonic, advanced by the wall clock), and - DEBUG - a vortex icon (kind-coloured cross, labelled by taxonomy, with a condensation bar and the multi-vortex N) at every active vortex's contact point, its funnel's collar table as a stack of wireframe rings, and dust devils as small tan crosses. Everything is squash-corrected through the cloud field and distance-thinned (ring segments and labels fall off with camera distance) - the camera shapes the DISPLAY only; every position drawn came from the scheduler's (or SSVortices') world-frame state. [interaction: SSVolCloud squashScale] [interaction: SSVortices active/dustDevils]
void SSAtmoInfoView::renderStormCells()
{
    const StormCellsData d = stormCellsData();
    if (!d.mValid) return;

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    if (!camera) return;
    const LLVector3 cam = camera->getOrigin();
    const F32 z = d.mLayerZ;

    const SSVolCloud* clouds = SSVolCloud::instanceExists() ? SSVolCloud::getInstance() : nullptr;
    auto drawn = [&](const LLVector3& p) -> LLVector3
    {
        if (!clouds) return p;
        const LLVector3 rel = p - cam;
        const F32 dist = rel.magVec();
        if (dist <= 1.0e-4f) return p;
        return cam + rel * clouds->squashScale(dist);
    };
    auto distXY = [&](const LLVector2& p) -> F32
    {
        const F32 dx = p.mV[0] - cam.mV[VX];
        const F32 dy = p.mV[1] - cam.mV[VY];
        return std::sqrt(dx * dx + dy * dy);
    };
    auto at = [&](const LLVector2& p, F32 dz = 0.f) -> LLVector3
    {
        return LLVector3(p.mV[0], p.mV[1], z + dz);
    };

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_FALSE, GL_FALSE); // <SS:Nexii> off, not just no-write: this draws in the 3-D pass AFTER SSAtmoInfoView::renderDimAndWorld has laid the dim quad down, on top like a Skylines layer, so it must never be occluded by world geometry
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    // Filled lattice tiles first (two triangles each), so every line lands on top of them. V2 lattice-tint
    // smoothing (doc/atmo_magic_phase8_show.md section 3 item 2): each tile's SPAWN-DECISION quantity is still one
    // flat SSStormCell::candidate().mPotential per lattice cell (t.mPotential, unchanged - the gate never reads a
    // corner) - but the TINT drawn for it is evaluated at the tile's four corners, each corner averaged over the
    // (up to 4) neighbouring tiles that share it, and painted as two per-vertex-coloured triangles so gGL's
    // hardware interpolation reads the potential field as a continuous surface across tile boundaries rather than
    // a flat colour per 2900 m cell. Corner (cx,cy) sits at world (cx*LATTICE_M, cy*LATTICE_M) and is shared by
    // tiles (cx-1,cy-1)/(cx,cy-1)/(cx-1,cy)/(cx,cy); a tile's own four corners are therefore (mLX,mLY),
    // (mLX+1,mLY), (mLX+1,mLY+1), (mLX,mLY+1). Tiles outside the enumerated field (e.g. just past FIELD_M) are
    // simply absent from the lookup, so an edge corner averages over whichever 1-3 neighbours ARE present - never a
    // fresh candidate() call outside the field, never a different potential than what mTiles already carries.
    const F32 half = SSStormCell::LATTICE_M * 0.47f;
    // <SS:Nexii> SSAtmoInfoViewTileTint (default off): skips the filled lattice tint and, further down, the tile
    // outline lines - `half` stays computed unconditionally since the outline loop still needs the tile's corner
    // geometry when tinting is on. Every other V2 entity (rings, hero ribbon, vortex icons, line band, pins,
    // labels) draws regardless.
    static LLCachedControl<bool> tile_tint(gSavedSettings, "SSAtmoInfoViewTileTint", false);
    if (tile_tint)
    {
        std::map<std::pair<S32, S32>, F32> tile_potential;
        for (const StormCellsData::Tile& t : d.mTiles)
        {
            tile_potential[std::make_pair(t.mLX, t.mLY)] = t.mPotential;
        }
        auto cornerColor = [&](S32 cx, S32 cy) -> LLColor4
        {
            F32 sum = 0.f;
            S32 n = 0;
            for (S32 dy = -1; dy <= 0; ++dy)
            {
                for (S32 dx = -1; dx <= 0; ++dx)
                {
                    const auto it = tile_potential.find(std::make_pair(cx + dx, cy + dy));
                    if (it != tile_potential.end())
                    {
                        sum += it->second;
                        ++n;
                    }
                }
            }
            const F32 p = (n > 0) ? (sum / (F32)n) : 0.f;
            return toColor(energyRamp(p, 1.f), latticeTileAlpha(p, SSStormCell::SPAWN_THRESHOLD));
        };
        gGL.begin(LLRender::TRIANGLES);
        for (const StormCellsData::Tile& t : d.mTiles)
        {
            const LLVector3 a = drawn(LLVector3(t.mCentreAgent.mV[0] - half, t.mCentreAgent.mV[1] - half, z));
            const LLVector3 b = drawn(LLVector3(t.mCentreAgent.mV[0] + half, t.mCentreAgent.mV[1] - half, z));
            const LLVector3 cc = drawn(LLVector3(t.mCentreAgent.mV[0] + half, t.mCentreAgent.mV[1] + half, z));
            const LLVector3 dd = drawn(LLVector3(t.mCentreAgent.mV[0] - half, t.mCentreAgent.mV[1] + half, z));
            const LLColor4 ca = cornerColor(t.mLX,     t.mLY);
            const LLColor4 cb = cornerColor(t.mLX + 1, t.mLY);
            const LLColor4 ccorner = cornerColor(t.mLX + 1, t.mLY + 1);
            const LLColor4 cd = cornerColor(t.mLX,     t.mLY + 1);
            gGL.color4fv(ca.mV);      gGL.vertex3fv(a.mV);
            gGL.color4fv(cb.mV);      gGL.vertex3fv(b.mV);
            gGL.color4fv(ccorner.mV); gGL.vertex3fv(cc.mV);
            gGL.color4fv(ca.mV);      gGL.vertex3fv(a.mV);
            gGL.color4fv(ccorner.mV); gGL.vertex3fv(cc.mV);
            gGL.color4fv(cd.mV);      gGL.vertex3fv(dd.mV);
        }
        gGL.end();
    }

    // <SS:Nexii> V2 LINE BAND (doc/atmo_magic_phase8_show.md section 3 item 1): the active squall line's own deck-
    // coupling geometry (SSStormCells::fillLineBand, read-only - d.mLineBand's own comment) drawn as two
    // translucent quads: the WALL (bandM either side of the segment, halfLen either way along it) and the SHELF (a
    // parallelogram spanned by dir x halfLen and mot x shelfM, matching SSStormCouple::lineField's own along/ahead
    // axes rather than assuming they are perpendicular - a stalled line's mot is (0,0) and the shelf quad
    // degenerates to a zero-area sliver, drawing nothing visible). This is a flat outline of the band's EXTENT for
    // orientation, not a re-rendering of lineField's own smoothstep falloff inside it; alpha scales with the
    // line's live strength so a weakening line fades rather than popping off.
    if (d.mLineBand.mStrength > 0.f && d.mLineBand.mBandM > 0.f && d.mLineBand.mHalfLenM > 0.f)
    {
        const LLVector2& o = d.mLineBand.mOriginAgent;
        const LLVector2& dir = d.mLineBand.mDir;
        const LLVector2 perp(-dir.mV[1], dir.mV[0]);
        const F32 halfLen = d.mLineBand.mHalfLenM;
        const F32 bandM = d.mLineBand.mBandM;
        const F32 strength = llclamp(d.mLineBand.mStrength, 0.f, 1.f);

        gGL.begin(LLRender::TRIANGLES);
        {
            const LLColor4 wc(LINE_WALL_FILL.mV[0], LINE_WALL_FILL.mV[1], LINE_WALL_FILL.mV[2], LINE_WALL_FILL.mV[3] * strength);
            gGL.color4fv(wc.mV);
            const LLVector3 p0 = drawn(at(LLVector2(o.mV[0] - dir.mV[0] * halfLen - perp.mV[0] * bandM, o.mV[1] - dir.mV[1] * halfLen - perp.mV[1] * bandM)));
            const LLVector3 p1 = drawn(at(LLVector2(o.mV[0] + dir.mV[0] * halfLen - perp.mV[0] * bandM, o.mV[1] + dir.mV[1] * halfLen - perp.mV[1] * bandM)));
            const LLVector3 p2 = drawn(at(LLVector2(o.mV[0] + dir.mV[0] * halfLen + perp.mV[0] * bandM, o.mV[1] + dir.mV[1] * halfLen + perp.mV[1] * bandM)));
            const LLVector3 p3 = drawn(at(LLVector2(o.mV[0] - dir.mV[0] * halfLen + perp.mV[0] * bandM, o.mV[1] - dir.mV[1] * halfLen + perp.mV[1] * bandM)));
            gGL.vertex3fv(p0.mV); gGL.vertex3fv(p1.mV); gGL.vertex3fv(p2.mV);
            gGL.vertex3fv(p0.mV); gGL.vertex3fv(p2.mV); gGL.vertex3fv(p3.mV);
        }
        if (d.mLineBand.mShelfM > 0.f)
        {
            const LLVector2& mot = d.mLineBand.mMotion;
            const F32 shelfM = d.mLineBand.mShelfM;
            const LLColor4 shc(LINE_SHELF_FILL.mV[0], LINE_SHELF_FILL.mV[1], LINE_SHELF_FILL.mV[2], LINE_SHELF_FILL.mV[3] * strength);
            gGL.color4fv(shc.mV);
            const LLVector3 p0 = drawn(at(LLVector2(o.mV[0] - dir.mV[0] * halfLen, o.mV[1] - dir.mV[1] * halfLen)));
            const LLVector3 p1 = drawn(at(LLVector2(o.mV[0] + dir.mV[0] * halfLen, o.mV[1] + dir.mV[1] * halfLen)));
            const LLVector3 p2 = drawn(at(LLVector2(o.mV[0] + dir.mV[0] * halfLen + mot.mV[0] * shelfM, o.mV[1] + dir.mV[1] * halfLen + mot.mV[1] * shelfM)));
            const LLVector3 p3 = drawn(at(LLVector2(o.mV[0] - dir.mV[0] * halfLen + mot.mV[0] * shelfM, o.mV[1] - dir.mV[1] * halfLen + mot.mV[1] * shelfM)));
            gGL.vertex3fv(p0.mV); gGL.vertex3fv(p1.mV); gGL.vertex3fv(p2.mV);
            gGL.vertex3fv(p0.mV); gGL.vertex3fv(p2.mV); gGL.vertex3fv(p3.mV);
        }
        gGL.end();
    }

    auto line = [&](const LLVector3& a, const LLVector3& b)
    {
        const LLVector3 span = b - a;
        const S32 segs = llclamp((S32)(span.magVec() / 200.f), 1, 24);
        LLVector3 prev = drawn(a);
        for (S32 i = 1; i <= segs; ++i)
        {
            const LLVector3 next = drawn(a + span * ((F32)i / (F32)segs));
            gGL.vertex3fv(prev.mV);
            gGL.vertex3fv(next.mV);
            prev = next;
        }
    };
    auto ring = [&](const LLVector2& centre, F32 radius, const LLColor4& color)
    {
        if (radius <= 0.f) return;
        const S32 n = ringSegments(radius, distXY(centre));
        gGL.color4fv(color.mV);
        LLVector3 prev = drawn(at(LLVector2(centre.mV[0] + radius, centre.mV[1])));
        for (S32 i = 1; i <= n; ++i)
        {
            const F32 a = (F32)i / (F32)n * TWO_PI_F;
            const LLVector3 next = drawn(at(LLVector2(centre.mV[0] + std::cos(a) * radius, centre.mV[1] + std::sin(a) * radius)));
            gGL.vertex3fv(prev.mV);
            gGL.vertex3fv(next.mV);
            prev = next;
        }
    };
    // Same as `ring` but at an explicit world Z rather than the layer's fixed z - the collar wireframe stacks one
    // of these per collar height (SSVortex::collarAltitudeM already returns world Z, so no further conversion).
    auto ringZ = [&](const LLVector2& centre, F32 radius, F32 zAt, const LLColor4& color)
    {
        if (radius <= 0.f) return;
        const S32 n = ringSegments(radius, distXY(centre));
        gGL.color4fv(color.mV);
        LLVector3 prev = drawn(LLVector3(centre.mV[0] + radius, centre.mV[1], zAt));
        for (S32 i = 1; i <= n; ++i)
        {
            const F32 a = (F32)i / (F32)n * TWO_PI_F;
            const LLVector3 next = drawn(LLVector3(centre.mV[0] + std::cos(a) * radius, centre.mV[1] + std::sin(a) * radius, zAt));
            gGL.vertex3fv(prev.mV);
            gGL.vertex3fv(next.mV);
            prev = next;
        }
    };
    auto cross = [&](const LLVector2& centre, F32 arm, const LLColor4& color)
    {
        gGL.color4fv(color.mV);
        line(at(LLVector2(centre.mV[0] - arm, centre.mV[1])), at(LLVector2(centre.mV[0] + arm, centre.mV[1])));
        line(at(LLVector2(centre.mV[0], centre.mV[1] - arm)), at(LLVector2(centre.mV[0], centre.mV[1] + arm)));
    };
    auto arrow = [&](const LLVector2& tail, const LLVector2& dir, F32 len, const LLColor4& color)
    {
        gGL.color4fv(color.mV);
        const LLVector2 tip(tail.mV[0] + dir.mV[0] * len, tail.mV[1] + dir.mV[1] * len);
        const LLVector2 perp(-dir.mV[1], dir.mV[0]);
        const LLVector2 barb_a(tip.mV[0] - dir.mV[0] * len * 0.3f + perp.mV[0] * len * 0.18f, tip.mV[1] - dir.mV[1] * len * 0.3f + perp.mV[1] * len * 0.18f);
        const LLVector2 barb_b(tip.mV[0] - dir.mV[0] * len * 0.3f - perp.mV[0] * len * 0.18f, tip.mV[1] - dir.mV[1] * len * 0.3f - perp.mV[1] * len * 0.18f);
        line(at(tail), at(tip));
        line(at(tip), at(barb_a));
        line(at(tip), at(barb_b));
    };

    gGL.begin(LLRender::LINES);

    // Tile outlines: faint everywhere, bright where an active cell was born. Gated by the same tile_tint setting
    // as the filled lattice tint above, so tile tint off means no lattice tile geometry at all.
    if (tile_tint)
    {
        for (const StormCellsData::Tile& t : d.mTiles)
        {
            const LLColor4& c = t.mAlive ? TILE_ALIVE : TILE_EDGE;
            gGL.color4fv(c.mV);
            const LLVector2 p0(t.mCentreAgent.mV[0] - half, t.mCentreAgent.mV[1] - half);
            const LLVector2 p1(t.mCentreAgent.mV[0] + half, t.mCentreAgent.mV[1] - half);
            const LLVector2 p2(t.mCentreAgent.mV[0] + half, t.mCentreAgent.mV[1] + half);
            const LLVector2 p3(t.mCentreAgent.mV[0] - half, t.mCentreAgent.mV[1] + half);
            line(at(p0), at(p1)); line(at(p1), at(p2)); line(at(p2), at(p3)); line(at(p3), at(p0));
        }
    }

    // The anchor: the weather domain's region centre, with the hero pass law as two faint rings - the law's own
    // median (HERO_PASS_MAX_M * 0.5^HERO_PASS_SKEW, ~300 m: half of all passes land inside this ring) and
    // HERO_PASS_MAX_M itself (the pass never exceeds this). HERO_PASS_MIN_M is 0 m and draws nothing (ring()
    // skips radius <= 0), so it is not one of the two rings.
    cross(d.mAnchorAgent, 120.f, ANCHOR_WHITE);
    ring(d.mAnchorAgent, SSStormCell::HERO_PASS_MAX_M * std::pow(0.5f, SSStormCell::HERO_PASS_SKEW), LLColor4(1.f, 1.f, 1.f, 0.25f));
    ring(d.mAnchorAgent, SSStormCell::HERO_PASS_MAX_M, LLColor4(1.f, 1.f, 1.f, 0.25f));

    // Alive cells: influence ring by stage, the 40% plateau inside it, a centre cross, and rotation glyphs for supercells.
    for (const StormCellsData::Cell& c : d.mCells)
    {
        const LLColor4 sc = toColor(stageColor(c.mStage), c.mIsHero ? 1.f : 0.85f);
        ring(c.mCentreAgent, c.mRadiusM, sc);
        // <SS:Nexii> review S10: the 40% plateau is SSStormCouple::INFLUENCE_CORE, not a second hand-spelled 0.4f - the ring drawn here must be the same radius SSStormCouple::influence treats as full influence, or the readout lies about where the plateau actually is.
        ring(c.mCentreAgent, c.mRadiusM * SSStormCouple::INFLUENCE_CORE, LLColor4(sc.mV[0], sc.mV[1], sc.mV[2], 0.35f));
        cross(c.mCentreAgent, llmax(40.f, c.mRadiusM * 0.06f), sc);

        if (c.mSupercell)
        {
            const LLColor4 rc = toColor(rotationRamp(c.mRotation), 0.95f);
            const F32 orbit = llmax(c.mRadiusM * 0.55f, 120.f);
            const F32 len = llmax(c.mRadiusM * 0.14f, 40.f) * llclamp(0.4f + 0.6f * c.mMeso, 0.4f, 1.f);
            const F32 sign = (c.mRotation < 0.f) ? -1.f : 1.f;
            for (S32 i = 0; i < ORBIT_GLYPHS; ++i)
            {
                const F32 a = orbitAngle(d.mNow, c.mRotation, i, ORBIT_GLYPHS, ORBIT_PERIOD_S);
                const LLVector2 tail(c.mCentreAgent.mV[0] + std::cos(a) * orbit, c.mCentreAgent.mV[1] + std::sin(a) * orbit);
                // Tangent: anticlockwise for positive omega, clockwise for negative.
                const LLVector2 dir(-std::sin(a) * sign, std::cos(a) * sign);
                arrow(tail, dir, len, rc);
            }
        }

        if (c.mIsForced)
        {
            // <SS:Nexii> FORCED: a distinct outline (a bright ring just outside the stage ring, with a dimmer one
            // outside that) marking the authored/forced pin (SSSquall::forcedCandidate) apart from every hashed
            // cell around it - a forced cell is never also a squall-line member (ssstormcellcore.h's own comment on
            // ActiveCell), so this never competes with the squall bar below for the same swatch.
            ring(c.mCentreAgent, c.mRadiusM * 1.06f, FORCED_OUTLINE);
            ring(c.mCentreAgent, c.mRadiusM * 1.16f, LLColor4(FORCED_OUTLINE.mV[0], FORCED_OUTLINE.mV[1], FORCED_OUTLINE.mV[2], 0.45f));
        }
    }

    // <SS:Nexii> SQUALL: the bar through a line's own currently-alive members, in the template's own along-the-line
    // order (stormCellsData()'s own comment), plus a marker at every leading-edge QLCS spin-up junction the
    // template defines - drawn whether or not its neighbouring members happen to be alive right now, a property of
    // the line's geometry rather than of which members gated this instant.
    for (const StormCellsData::SquallLine& sl : d.mSquallLines)
    {
        gGL.color4fv(SQUALL_LINE.mV);
        for (size_t i = 0; i + 1 < sl.mMembersAgent.size(); ++i)
        {
            line(at(sl.mMembersAgent[i]), at(sl.mMembersAgent[i + 1]));
        }
        for (const LLVector2& j : sl.mJunctionsAgent)
        {
            cross(j, 90.f, SQUALL_JUNCTION);
        }
    }

    // The hero ribbon: origin -> death, coloured by the lifecycle stage each stretch falls in, with age ticks; a marker at now; the closest approach tied to the anchor.
    if (d.mHaveHero)
    {
        const StormCellsData::HeroPath& h = d.mHero;
        const F32 life = llmax(h.mLifetimeS, 1.f);
        const LLVector2 span(h.mDeathAgent.mV[0] - h.mOriginAgent.mV[0], h.mDeathAgent.mV[1] - h.mOriginAgent.mV[1]);
        const F32 span_len = span.length();
        const LLVector2 dir = (span_len > 1e-3f) ? LLVector2(span.mV[0] / span_len, span.mV[1] / span_len) : LLVector2(0.f, 1.f);
        const LLVector2 perp(-dir.mV[1], dir.mV[0]);

        const S32 strips = 40;
        const F32 age_now = (F32)((d.mNow - h.mBirthTime) / (F64)life);
        for (S32 i = 0; i < strips; ++i)
        {
            const F32 t0 = (F32)i / (F32)strips;
            const F32 t1 = (F32)(i + 1) / (F32)strips;
            const LLVector2 a(h.mOriginAgent.mV[0] + span.mV[0] * t0, h.mOriginAgent.mV[1] + span.mV[1] * t0);
            const LLVector2 b(h.mOriginAgent.mV[0] + span.mV[0] * t1, h.mOriginAgent.mV[1] + span.mV[1] * t1);
            const bool past = t1 <= age_now;
            gGL.color4fv(toColor(lifecycleRamp((t0 + t1) * 0.5f), past ? 1.f : 0.45f).mV);
            line(at(a), at(b));
        }

        // Age ticks, perpendicular, one every ribbonTickIntervalS of lifetime.
        const F32 tick_s = ribbonTickIntervalS(h.mLifetimeS);
        const F32 tick_len = 60.f;
        for (F32 s_age = tick_s; s_age < h.mLifetimeS; s_age += tick_s)
        {
            const F32 t = s_age / life;
            const LLVector2 p(h.mOriginAgent.mV[0] + span.mV[0] * t, h.mOriginAgent.mV[1] + span.mV[1] * t);
            gGL.color4fv(toColor(lifecycleRamp(t), 0.9f).mV);
            line(at(LLVector2(p.mV[0] - perp.mV[0] * tick_len, p.mV[1] - perp.mV[1] * tick_len)),
                 at(LLVector2(p.mV[0] + perp.mV[0] * tick_len, p.mV[1] + perp.mV[1] * tick_len)));
        }

        cross(h.mOriginAgent, 150.f, HERO_ORIGIN);
        cross(h.mDeathAgent, 150.f, HERO_DEATH);
        // Now: a diamond.
        {
            const F32 r = 110.f;
            gGL.color4fv(ANCHOR_WHITE.mV);
            const LLVector2 n = h.mNowAgent;
            line(at(LLVector2(n.mV[0] - r, n.mV[1])), at(LLVector2(n.mV[0], n.mV[1] + r)));
            line(at(LLVector2(n.mV[0], n.mV[1] + r)), at(LLVector2(n.mV[0] + r, n.mV[1])));
            line(at(LLVector2(n.mV[0] + r, n.mV[1])), at(LLVector2(n.mV[0], n.mV[1] - r)));
            line(at(LLVector2(n.mV[0], n.mV[1] - r)), at(LLVector2(n.mV[0] - r, n.mV[1])));
        }
        // Closest approach (7f V2 item 3): a ring at the anchor of radius mClosestDistM - the ACTUAL measured pass
        // distance for THIS hero, distinct from the two static design-limit band rings drawn at the anchor above
        // (HERO_PASS_MIN_M/MAX_M) - plus a marker at h.mClosestAgent, the CONTACT PATH's own closest point (composeHero
        // composes the path to the FUNNEL's contact, not the cell centre - see ssstormcellcore.h's composeHero), and
        // the perpendicular from that point to the anchor.
        ring(d.mAnchorAgent, h.mClosestDistM, LLColor4(1.f, 1.f, 1.f, 0.5f));
        ring(h.mClosestAgent, 90.f, ANCHOR_WHITE);
        gGL.color4fv(LLColor4(1.f, 1.f, 1.f, 0.6f).mV);
        line(at(h.mClosestAgent), at(d.mAnchorAgent));
        // Motion arrow from the origin.
        const F32 speed = h.mMotion.length();
        if (speed > 1e-3f)
        {
            arrow(h.mOriginAgent, LLVector2(h.mMotion.mV[0] / speed, h.mMotion.mV[1] / speed), 400.f, HERO_ORIGIN);
        }
    }

    // <SS:Nexii> DEBUG: vortex icons - a kind-coloured cross at each active vortex's contact point (colour =
    // vortexKindColor, sign-only rotation ramp), a condensation bar beside it (grey trough, filled to
    // mCondensation in the kind colour), and - funnel-having kinds only - the funnel's own collar table drawn as a
    // stack of wireframe rings, squash-corrected through `drawn` exactly like every other ring in this layer (the
    // camera shapes the DISPLAY only; every collar radius/altitude came from SSVortices' world-frame state).
    // [interaction: SSVortices active]
    for (const StormCellsData::VortexIcon& v : d.mVortices)
    {
        const LLColor4 kc = toColor(vortexKindColor(v.mRotationSign), v.mParentIsHero ? 1.f : 0.85f);
        const F32 arm = llclamp(90.f * llmax(v.mIntensity, 0.2f), 30.f, 140.f);
        cross(v.mContactAgent, arm, kc);

        // Condensation bar: a fixed-height trough beside the icon, filled bottom-up to mCondensation.
        const F32 bar_h = 260.f;
        const LLVector2 bar_pos(v.mContactAgent.mV[0] + arm * 1.6f, v.mContactAgent.mV[1]);
        gGL.color4fv(LLColor4(kc.mV[0], kc.mV[1], kc.mV[2], 0.25f).mV);
        line(at(bar_pos), at(bar_pos, bar_h));
        gGL.color4fv(kc.mV);
        line(at(bar_pos), at(bar_pos, bar_h * llclamp(v.mCondensation, 0.f, 1.f)));

        // Collar wireframe: one ring per collar, at its own world Z.
        if (v.mHasFunnel)
        {
            const LLColor4 collar_c(kc.mV[0], kc.mV[1], kc.mV[2], 0.35f);
            for (const StormCellsData::VortexIcon::Collar& col : v.mCollars)
            {
                ringZ(v.mContactAgent, col.mRadiusM, col.mAltitudeZ, collar_c);
            }
        }
    }

    // Dust devils: parentless, funnel-less - a small dusty-tan cross at the origin, no collar table.
    for (const StormCellsData::DustIcon& dv : d.mDustDevils)
    {
        cross(dv.mOriginAgent, llclamp(60.f * llmax(dv.mIntensity, 0.2f), 20.f, 90.f), DUST_COLOR);
    }

    gGL.end();

    // Labels, thinned by camera distance: tiles within 5 km show P, cells within 9 km their id/stage/age, the hero and anchor always.
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    if (font)
    {
        for (const StormCellsData::Tile& t : d.mTiles)
        {
            if (distXY(t.mCentreAgent) > 5000.f) continue;
            const LLColor4 c = toColor(energyRamp(t.mPotential, 1.f), 0.9f);
            hud_render_utf8text(llformat("P %.2f  SH %.2f", t.mPotential, t.mShearNoise), drawn(at(t.mCentreAgent)), *font,
                                LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, -30.f, -4.f, c, false);
        }
        for (const StormCellsData::Cell& c : d.mCells)
        {
            if (!c.mIsHero && distXY(c.mCentreAgent) > 9000.f) continue;
            const F64 age = d.mNow - c.mBirthTime;
            const std::string rot = c.mSupercell ? llformat("  rot %+.2f", c.mRotation) : std::string();
            const std::string tag = c.mIsForced ? "  FORCED" : (c.mLineId != 0 ? llformat("  line %s", shortId(c.mLineId).c_str()) : std::string());
            const std::string label = llformat("%s%s %s  %.0f/%.0f min  r %.0f%s  I %.2f%s", c.mIsHero ? "HERO " : (c.mSupercell ? "SUPER " : ""),
                                               shortId(c.mId).c_str(), stageLabel(c.mStage), age / 60.0, c.mLifetimeS / 60.f, c.mRadiusM,
                                               rot.c_str(), c.mIntensity, tag.c_str());
            const LLColor4 label_c = c.mIsForced ? FORCED_OUTLINE : toColor(stageColor(c.mStage), 1.f);
            hud_render_utf8text(label, drawn(at(c.mCentreAgent, 30.f)), *font, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, 8.f, 6.f,
                                label_c, false);
        }
        hud_render_utf8text("anchor (region centre)", drawn(at(d.mAnchorAgent)), *font, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, 8.f, -14.f, ANCHOR_WHITE, false);
        if (d.mHaveHero)
        {
            const StormCellsData::HeroPath& h = d.mHero;
            hud_render_utf8text(llformat("hero origin  %.0f m upwind", (h.mOriginAgent - h.mClosestAgent).length()), drawn(at(h.mOriginAgent)), *font,
                                LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, 8.f, 6.f, HERO_ORIGIN, false);
            hud_render_utf8text(llformat("closest %.0f m  %+.0f s", h.mClosestDistM, h.mClosestTime - d.mNow), drawn(at(h.mClosestAgent)), *font,
                                LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, 8.f, 6.f, ANCHOR_WHITE, false);
            hud_render_utf8text(llformat("death  +%.0f min", (h.mBirthTime + (F64)h.mLifetimeS - d.mNow) / 60.0), drawn(at(h.mDeathAgent)), *font,
                                LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, 8.f, 6.f, HERO_DEATH, false);
        }

        // Vortex icons: kind, condensation, live intensity, and the multi-vortex N (0 = single vortex).
        for (const StormCellsData::VortexIcon& v : d.mVortices)
        {
            const LLColor4 c = toColor(vortexKindColor(v.mRotationSign), 1.f);
            std::string label = llformat("%s%s%s  cond %.2f  I %.2f", v.mParentIsHero ? "HERO " : "", vortexKindLabel(v.mKind),
                                         v.mWasRelabelledWaterspout ? " (relabelled)" : "", v.mCondensation, v.mIntensity);
            if (v.mMultiN > 0)
            {
                label += llformat("  N %d", v.mMultiN);
            }
            hud_render_utf8text(label, drawn(at(v.mContactAgent, 20.f)), *font, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, 8.f, 6.f, c, false);
        }
        for (const StormCellsData::DustIcon& dv : d.mDustDevils)
        {
            if (distXY(dv.mOriginAgent) > 5000.f) continue;
            hud_render_utf8text(llformat("dust devil  I %.2f", dv.mIntensity), drawn(at(dv.mOriginAgent)), *font,
                                LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, 8.f, 6.f, DUST_COLOR, false);
        }
    }
}

// ---------------------------------------------------------------------------
// The in-world layer: V3's deck LOD
// ---------------------------------------------------------------------------

// The V3 layer (ssdecklodcore.h CONTRACT): five rings about the camera on the deck plane, one per LOD rail
// (SUBS_FULL_M, SUBS_TWO_M, THIN_START_M, FIELD_FADE_START_M, DECK_EDGE_M), and a coarse grid of tinted tiles
// (grey ramp, SSDeckLod::keepFrac at each tile's own camera distance). This is a DIAGRAM of the distance-only LOD
// ramp, not a replay of buildDeck's own cell gate/occupancy - the view has no access to that state and the
// contract forbids reimplementing it here, so the tile grid is a fixed pitch centred on the camera rather than
// the builder's own drifting, hero-shifted, gated lattice. Squash-corrected like every other in-world layer.
// [interaction: SSVolCloud squashScale/cloudBaseZ/puffCount]
void SSAtmoInfoView::renderDeckLod()
{
    const DeckLodData d = deckLodData();

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    if (!camera) return;
    const LLVector3 cam = camera->getOrigin();
    const F32 z = d.mLayerZ;

    const SSVolCloud* clouds = SSVolCloud::instanceExists() ? SSVolCloud::getInstance() : nullptr;
    auto drawn = [&](const LLVector3& p) -> LLVector3
    {
        if (!clouds) return p;
        const LLVector3 rel = p - cam;
        const F32 dist = rel.magVec();
        if (dist <= 1.0e-4f) return p;
        return cam + rel * clouds->squashScale(dist);
    };

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_FALSE, GL_FALSE); // <SS:Nexii> off, not just no-write: this draws in the 3-D pass AFTER SSAtmoInfoView::renderDimAndWorld has laid the dim quad down, on top like a Skylines layer, so it must never be occluded by world geometry
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    // Tinted tiles: a fixed pitch (coarser than the builder's own SSDeckLod::CELL_M, since this is a display
    // sampling of a pure distance function, not a cell-for-cell replay), covering the ramp's whole reach.
    // <SS:Nexii> SSAtmoInfoViewTileTint (default off): skips just this keep-fraction grid so the rails, their
    // labels and the Tier B macro grid below (a distinct diagram, untouched by this setting) still read - the
    // user's complaint was the tile cells burying the rails, not the macro grid.
    const F32 reach = SSDeckLod::DECK_EDGE_M;
    static LLCachedControl<bool> tile_tint(gSavedSettings, "SSAtmoInfoViewTileTint", false);
    if (tile_tint)
    {
        const F32 pitch = SSDeckLod::CELL_M * 4.f;
        const S32 half_n = (S32)std::ceil(reach / pitch);
        const S32 cx0 = (S32)std::floor(cam.mV[VX] / pitch);
        const S32 cy0 = (S32)std::floor(cam.mV[VY] / pitch);
        const F32 half_tile = pitch * 0.47f;

        gGL.begin(LLRender::TRIANGLES);
        for (S32 ty = -half_n; ty <= half_n; ++ty)
        {
            for (S32 tx = -half_n; tx <= half_n; ++tx)
            {
                const F32 tcx = (F32)(cx0 + tx) * pitch + pitch * 0.5f;
                const F32 tcy = (F32)(cy0 + ty) * pitch + pitch * 0.5f;
                const F32 ddx = tcx - cam.mV[VX];
                const F32 ddy = tcy - cam.mV[VY];
                const F32 dist = std::sqrt(ddx * ddx + ddy * ddy);
                if (dist > reach) continue;

                const LLColor4 tint = toColor(presenceRamp(SSDeckLod::keepFrac(dist), 1.f), 0.30f);
                gGL.color4fv(tint.mV);
                const LLVector3 a = drawn(LLVector3(tcx - half_tile, tcy - half_tile, z));
                const LLVector3 b = drawn(LLVector3(tcx + half_tile, tcy - half_tile, z));
                const LLVector3 c = drawn(LLVector3(tcx + half_tile, tcy + half_tile, z));
                const LLVector3 e = drawn(LLVector3(tcx - half_tile, tcy + half_tile, z));
                gGL.vertex3fv(a.mV); gGL.vertex3fv(b.mV); gGL.vertex3fv(c.mV);
                gGL.vertex3fv(a.mV); gGL.vertex3fv(c.mV); gGL.vertex3fv(e.mV);
            }
        }
        gGL.end();
    }

    // <SS:Nexii> LOD phase 6d (ssdeckmacrocore.h CONTRACT): the macro grid, a coarser MACRO_M-pitch tint drawn
    // ONLY beyond the same TIER_B_M - TIER_BLEND_M/2 rail buildDeck's own accumulator uses (macroEligible's own
    // cellInWalk-style membership on the tile centre is display-only here, same caveat as the fine tile grid
    // above - a diagram of the distance ramp, not a replay of buildDeck's own occupancy), alpha carrying wB
    // (SSDeckMacro::tierWeights) so the tint itself crossfades in across the blend band the way Tier B's real
    // bodies do.
    const F32 macro_pitch = SSDeckMacro::MACRO_M;
    const S32 macro_half_n = (S32)std::ceil(reach / macro_pitch);
    const S32 macro_cx0 = (S32)std::floor(cam.mV[VX] / macro_pitch);
    const S32 macro_cy0 = (S32)std::floor(cam.mV[VY] / macro_pitch);
    const F32 macro_half_tile = macro_pitch * 0.47f;
    const F32 macro_lo = SSDeckMacro::TIER_B_M - SSDeckMacro::TIER_BLEND_M * 0.5f;

    gGL.begin(LLRender::TRIANGLES);
    for (S32 ty = -macro_half_n; ty <= macro_half_n; ++ty)
    {
        for (S32 tx = -macro_half_n; tx <= macro_half_n; ++tx)
        {
            const F32 tcx = (F32)(macro_cx0 + tx) * macro_pitch + macro_pitch * 0.5f;
            const F32 tcy = (F32)(macro_cy0 + ty) * macro_pitch + macro_pitch * 0.5f;
            const F32 ddx = tcx - cam.mV[VX];
            const F32 ddy = tcy - cam.mV[VY];
            const F32 dist = std::sqrt(ddx * ddx + ddy * ddy);
            if (dist <= macro_lo || dist > reach) continue;

            F32 wA, wB;
            SSDeckMacro::tierWeights(dist, wA, wB);
            const LLColor4 tint(RING_TIER_B.mV[0], RING_TIER_B.mV[1], RING_TIER_B.mV[2], 0.30f * wB);
            gGL.color4fv(tint.mV);
            const LLVector3 a = drawn(LLVector3(tcx - macro_half_tile, tcy - macro_half_tile, z));
            const LLVector3 b = drawn(LLVector3(tcx + macro_half_tile, tcy - macro_half_tile, z));
            const LLVector3 c = drawn(LLVector3(tcx + macro_half_tile, tcy + macro_half_tile, z));
            const LLVector3 e = drawn(LLVector3(tcx - macro_half_tile, tcy + macro_half_tile, z));
            gGL.vertex3fv(a.mV); gGL.vertex3fv(b.mV); gGL.vertex3fv(c.mV);
            gGL.vertex3fv(a.mV); gGL.vertex3fv(c.mV); gGL.vertex3fv(e.mV);
        }
    }
    gGL.end();

    // The five rings, one per rail, centred on the camera - a fixed segment count rather than ringSegments'
    // apparent-size heuristic (that helper thins by distance FROM the camera TO a ring's centre, which here is
    // always zero; a flat 64 keeps every rail smooth without it).
    auto ring = [&](F32 radius, const LLColor4& color)
    {
        constexpr S32 n = 64;
        gGL.color4fv(color.mV);
        LLVector3 prev = drawn(LLVector3(cam.mV[VX] + radius, cam.mV[VY], z));
        for (S32 i = 1; i <= n; ++i)
        {
            const F32 a = (F32)i / (F32)n * TWO_PI_F;
            const LLVector3 next = drawn(LLVector3(cam.mV[VX] + std::cos(a) * radius, cam.mV[VY] + std::sin(a) * radius, z));
            gGL.vertex3fv(prev.mV);
            gGL.vertex3fv(next.mV);
            prev = next;
        }
    };

    gGL.begin(LLRender::LINES);
    ring(SSDeckLod::SUBS_FULL_M, RING_SUBS_FULL);
    ring(SSDeckLod::SUBS_TWO_M, RING_SUBS_TWO);
    ring(SSDeckLod::THIN_START_M, RING_THIN_START);
    ring(SSDeckLod::FIELD_FADE_START_M, RING_FADE_START);
    ring(SSDeckLod::DECK_EDGE_M, RING_DECK_EDGE);
    gGL.end();

    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    if (font)
    {
        auto label = [&](F32 radius, const char* text, const LLColor4& color)
        {
            hud_render_utf8text(text, drawn(LLVector3(cam.mV[VX] + radius, cam.mV[VY], z)), *font, LLFontGL::NORMAL,
                                LLFontGL::DROP_SHADOW, 6.f, 4.f, color, false);
        };
        label(SSDeckLod::SUBS_FULL_M, "subs full", RING_SUBS_FULL);
        label(SSDeckLod::SUBS_TWO_M, "subs 2->1", RING_SUBS_TWO);
        label(SSDeckLod::THIN_START_M, "thin start", RING_THIN_START);
        label(SSDeckLod::FIELD_FADE_START_M, "edge fade start", RING_FADE_START);
        label(SSDeckLod::DECK_EDGE_M, "deck edge", RING_DECK_EDGE);
    }
}

// ---------------------------------------------------------------------------
// The in-world layer: V4's precip & virga
// ---------------------------------------------------------------------------

// The V4 layer (doc/atmo_magic_debug_views.md V4, ssvirgacore.h CONTRACT): every qualifying cell from the LAST
// build's snapshot (SSAtmoInfoView::virgaData(), itself SSVolCloud::virgaDebug() read straight across) outlined
// at the EMBEDDED stack's top (mEmbedTopZ, F9 2026-09-06 review: not the deck base - phase 8e starts the stack
// above the base, inside the cloud) and tinted by drive (presence ramp - grey to white, per the design's colour
// language), bright for the cells the hashed trim actually KEPT and dim for the ones MAX_SHAFTS trimmed away; a
// fall-tilt comparison line per kept cell - deck base entry point to landing - using precip's OWN wind-tilt
// formula (SSAtmoInfoViewCore::fallTiltOffsetM); a SEPARATE skew POLYLINE (F9, 8e-b PROFILE SKEW 2026-09-06
// review) showing the curtain's OWN wind-fall lean - one chord per card boundary from the embedded stack's top
// down to the ground, each point SSVirga::profileSkewM(mWindParams, mBaseAglM, aglOfBoundary, mFallSpeed) at that
// boundary's own AGL height (the emitter's snapshotted inputs, never re-derived), tracing the real wind-profile
// integrated curve rather than one straight line, so the two lines are no longer describing the same, unleaning
// shape - and can be read against each other; and the particle rain's own handoff boundary (r2 * SSVirga::HANDOFF_SKIP) as a ring on the
// ground plane about the CAMERA, with its ramp
// band (HANDOFF_BAND_M) drawn as a short run of fading rings out to full shaft alpha. Squash-corrected like every
// other in-world layer. [interaction: SSVolCloud squashScale/virgaDebug]
void SSAtmoInfoView::renderVirga()
{
    const VirgaData d = virgaData();
    if (!d.mValid || !d.mActive) return;

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    if (!camera) return;
    const LLVector3 cam = camera->getOrigin();

    const SSVolCloud* clouds = SSVolCloud::instanceExists() ? SSVolCloud::getInstance() : nullptr;
    auto drawn = [&](const LLVector3& p) -> LLVector3
    {
        if (!clouds) return p;
        const LLVector3 rel = p - cam;
        const F32 dist = rel.magVec();
        if (dist <= 1.0e-4f) return p;
        return cam + rel * clouds->squashScale(dist);
    };

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_FALSE, GL_FALSE); // <SS:Nexii> off, not just no-write: this draws in the 3-D pass AFTER SSAtmoInfoView::renderDimAndWorld has laid the dim quad down, on top like a Skylines layer, so it must never be occluded by world geometry
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    auto line = [&](const LLVector3& a, const LLVector3& b)
    {
        const LLVector3 span = b - a;
        const S32 segs = llclamp((S32)(span.magVec() / 200.f), 1, 24);
        LLVector3 prev = drawn(a);
        for (S32 i = 1; i <= segs; ++i)
        {
            const LLVector3 next = drawn(a + span * ((F32)i / (F32)segs));
            gGL.vertex3fv(prev.mV);
            gGL.vertex3fv(next.mV);
            prev = next;
        }
    };
    // A ring centred on the camera at the layer's own z - the same fixed-64-segment idiom renderDeckLod uses for
    // its camera-centred rails (ringSegments' apparent-size heuristic thins by distance TO a ring's centre, which
    // here is always zero).
    auto camRing = [&](F32 radius, F32 zAt, const LLColor4& color)
    {
        if (radius <= 0.f) return;
        constexpr S32 n = 64;
        gGL.color4fv(color.mV);
        LLVector3 prev = drawn(LLVector3(cam.mV[VX] + radius, cam.mV[VY], zAt));
        for (S32 i = 1; i <= n; ++i)
        {
            const F32 a = (F32)i / (F32)n * TWO_PI_F;
            const LLVector3 next = drawn(LLVector3(cam.mV[VX] + std::cos(a) * radius, cam.mV[VY] + std::sin(a) * radius, zAt));
            gGL.vertex3fv(prev.mV);
            gGL.vertex3fv(next.mV);
            prev = next;
        }
    };

    gGL.begin(LLRender::LINES);

    // <SS:Nexii> F9 (2026-09-06 review): the qualifying cells, outlined at the EMBEDDED stack's own top
    // (mEmbedTopZ), not the deck base - phase 8e's stack starts inside the cloud, above the base, so this is
    // where the card stack the emitter actually built begins. Bright/kept, dim/trimmed.
    const F32 half = SSVirga::CELL_M * 0.45f;
    for (const VirgaData::Cell& c : d.mCells)
    {
        const LLColor4 col = toColor(presenceRamp(c.mDrive, 1.f), c.mKept ? 0.85f : 0.25f);
        gGL.color4fv(col.mV);
        const LLVector3 p0(c.mX - half, c.mY - half, d.mEmbedTopZ);
        const LLVector3 p1(c.mX + half, c.mY - half, d.mEmbedTopZ);
        const LLVector3 p2(c.mX + half, c.mY + half, d.mEmbedTopZ);
        const LLVector3 p3(c.mX - half, c.mY + half, d.mEmbedTopZ);
        line(p0, p1); line(p1, p2); line(p2, p3); line(p3, p0);
    }

    // Fall-tilt comparison lines, kept cells only (the ones actually drawn as shaft cards): landing at the cell's
    // OWN unshifted ground point, entry upwind at the deck base by precip's OWN ground-wind wind-tilt offset - a
    // display approximation (mWindGround/mFallSpeedMS, see virgaData's own comment), read against the skew line
    // below rather than against the curtain directly (F9, 2026-09-06 review: the curtain's own ground point is no
    // longer this same x/y - phase 8e gave it a real per-altitude lean).
    const F32 drop_h = llmax(d.mBaseZ - d.mGroundZ, 0.f);
    const Vec2M tilt = fallTiltOffsetM(d.mWindGround.x, d.mWindGround.y, d.mFallSpeedMS, drop_h);
    for (const VirgaData::Cell& c : d.mCells)
    {
        if (!c.mKept) continue;
        gGL.color4fv(RAIL_BASE.mV);
        const LLVector3 landing(c.mX, c.mY, d.mGroundZ);
        const LLVector3 entry(c.mX - tilt.x, c.mY - tilt.y, d.mBaseZ);
        line(entry, landing);
    }

    // <SS:Nexii> F9 (2026-09-06 review), 8e-b PROFILE SKEW: the curtain's OWN skew, kept cells only - a POLYLINE
    // of per-card chords, one vertex per card boundary from the embedded stack's top (mEmbedTopZ) down to the
    // ground, each vertex SSVirga::profileSkewM(mWindParams, mBaseAglM, aglOfBoundary, mFallSpeed) evaluated at
    // that boundary's own AGL height (baseAglM - (baseZ - z)) - the SAME pure function and the SAME snapshotted
    // params/baseAglM/fallSpeed cardGeom evaluated this build with (never re-derived), so this line is exactly
    // the curve the curtain's own cards actually skew along, not the old single-sample straight line.
    for (const VirgaData::Cell& c : d.mCells)
    {
        if (!c.mKept) continue;
        gGL.color4fv(VIRGA_SKEW.mV);
        const S32 nCards = SSVirga::cardCountOverlapped(d.mEmbedTopZ, d.mGroundZ);
        bool first = true;
        LLVector3 prev_pt(c.mX, c.mY, d.mEmbedTopZ);
        for (S32 k = 0; k < nCards; ++k)
        {
            const SSVirga::CardSpan cs = SSVirga::cardSpan(k, d.mEmbedTopZ, d.mGroundZ);
            if (first)
            {
                const F32 aglTop = d.mBaseAglM - (d.mBaseZ - cs.zTop);
                const SSVirga::Vec2 skewTop = SSVirga::profileSkewM(d.mWindParams, d.mBaseAglM, aglTop, d.mFallSpeed);
                prev_pt = LLVector3(c.mX + skewTop.x, c.mY + skewTop.y, cs.zTop);
                first = false;
            }
            const F32 aglBot = d.mBaseAglM - (d.mBaseZ - cs.zBot);
            const SSVirga::Vec2 skewBot = SSVirga::profileSkewM(d.mWindParams, d.mBaseAglM, aglBot, d.mFallSpeed);
            const LLVector3 bot_pt(c.mX + skewBot.x, c.mY + skewBot.y, cs.zBot);
            line(prev_pt, bot_pt);
            prev_pt = bot_pt;
        }
    }

    // The handoff ring and its ramp band, on the ground plane, centred on the camera.
    if (d.mHandoffRadius > 0.f)
    {
        camRing(d.mHandoffRadius, d.mGroundZ, VIRGA_HANDOFF);
        constexpr S32 BAND_STEPS = 5;
        for (S32 i = 1; i <= BAND_STEPS; ++i)
        {
            const F32 radius = d.mHandoffRadius + SSVirga::HANDOFF_BAND_M * (F32)i / (F32)BAND_STEPS;
            const F32 alpha = VIRGA_HANDOFF.mV[3] * SSVirga::handoff(radius, d.mR2);
            camRing(radius, d.mGroundZ, LLColor4(VIRGA_HANDOFF.mV[0], VIRGA_HANDOFF.mV[1], VIRGA_HANDOFF.mV[2], alpha));
        }
    }

    gGL.end();

    // Labels: drive at every kept cell (within 6 km), the handoff radius once.
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    if (font)
    {
        for (const VirgaData::Cell& c : d.mCells)
        {
            if (!c.mKept) continue;
            const F32 dx = c.mX - cam.mV[VX];
            const F32 dy = c.mY - cam.mV[VY];
            if (dx * dx + dy * dy > 6000.f * 6000.f) continue;
            const LLColor4 col = toColor(presenceRamp(c.mDrive, 1.f), 1.f);
            hud_render_utf8text(llformat("drive %.2f", c.mDrive), drawn(LLVector3(c.mX, c.mY, d.mBaseZ)), *font,
                                LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, 6.f, 4.f, col, false);
        }
        if (d.mHandoffRadius > 0.f)
        {
            hud_render_utf8text(llformat("handoff  r2 x %.1f = %.0f m", SSVirga::HANDOFF_SKIP, d.mHandoffRadius),
                                drawn(LLVector3(cam.mV[VX] + d.mHandoffRadius, cam.mV[VY], d.mGroundZ)), *font,
                                LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, 6.f, 4.f, VIRGA_HANDOFF, false);
        }
    }
}

// ---------------------------------------------------------------------------
// The in-world layer: V10's acoustics
// ---------------------------------------------------------------------------

// <SS:Nexii> The V10 layer (doc/atmo_magic_acoustics.md, the debug ask - the storm
// cells' info-view treatment applied to the acoustic channel): the baked probe set
// drawn as the geometry the store holds, the propagation graph between probes, the
// listener's own blend, and the last thunder's occlusion story - the DIRECT line the
// straight-line distance assumes (green where the span store says it is clear, red
// where it crosses solid) against the PROPAGATED path the graph actually routes
// (around buildings, through alleys, into courtyards), with the portal hops marked
// and the delay/muffle figures labelled at the source. Probe markers take the world
// field overlay's own air-state colours (outdoors green, sheltered amber, interior
// red) sized by the baked room volume, so the two readouts agree. Read-only over the
// bake - this never asks the field to build, flood or re-solve anything; with no
// current bake it draws nothing and the legend says why. The warm-gray LOOK pass
// behind it is the shared info-view dim (SSAtmoInfoViewLook), same as every mode.
void SSAtmoInfoView::renderAcoustics()
{
    const AcousticsData d = acousticsData();
    if (!d.mValid) return;

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    if (!camera) return;

    const LLVector3 cam = camera->getOrigin();

    const SSWorldField::AcousticDebug& fd = d.mField;

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_FALSE, GL_FALSE); // on top like a Skylines layer, never occluded
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    const LLColor4 LINK_AIR   (0.55f, 0.60f, 0.70f, 0.16f);
    const LLColor4 LINK_PORTAL(1.00f, 0.85f, 0.30f, 0.65f);
    const LLColor4 BLEND_LINE (0.40f, 0.70f, 1.00f, 0.80f);
    const LLColor4 PATH_CLEAR (0.35f, 1.00f, 0.45f, 0.55f);
    const LLColor4 PATH_OCCL  (1.00f, 0.25f, 0.20f, 0.60f);
    const LLColor4 PROP_PATH  (0.20f, 0.95f, 1.00f, 0.90f);
    const LLColor4 PORTAL_HOP (1.00f, 0.85f, 0.30f, 0.95f);

    auto probe_label_color = [&](const SSWorldField::AcousticDebug::Probe& p)
    {
        return toColor(airLabelColor((S32)p.mLabel), 0.9f);
    };

    gGL.begin(LLRender::LINES);

    // The graph: every link between the drawn probes. Portal edges bright - they are
    // the edges thunder and the muffle question care about.
    for (const SSWorldField::AcousticDebug::Link& l : fd.mLinks)
    {
        if (l.mA < 0 || l.mB >= (S32)fd.mProbes.size()) continue;
        gGL.color4fv((l.mPortal ? LINK_PORTAL : LINK_AIR).mV);
        gGL.vertex3fv(fd.mProbes[(size_t)l.mA].mPos.mV);
        gGL.vertex3fv(fd.mProbes[(size_t)l.mB].mPos.mV);
    }

    // The probes: a cross at ear height coloured by air state, sized by the baked
    // room volume, plus a faint stem spanning the probe's own gap so a stacked
    // storey reads as a stack.
    for (const SSWorldField::AcousticDebug::Probe& p : fd.mProbes)
    {
        const F32 s = probeMarkerM(p.mVolume);
        const LLColor4 c = probe_label_color(p);

        gGL.color4fv(c.mV);
        gGL.vertex3f(p.mPos.mV[VX] - s, p.mPos.mV[VY], p.mPos.mV[VZ]);
        gGL.vertex3f(p.mPos.mV[VX] + s, p.mPos.mV[VY], p.mPos.mV[VZ]);
        gGL.vertex3f(p.mPos.mV[VX], p.mPos.mV[VY] - s, p.mPos.mV[VZ]);
        gGL.vertex3f(p.mPos.mV[VX], p.mPos.mV[VY] + s, p.mPos.mV[VZ]);

        gGL.color4f(c.mV[0], c.mV[1], c.mV[2], 0.20f);
        gGL.vertex3f(p.mPos.mV[VX], p.mPos.mV[VY], p.mGapBottom);
        gGL.vertex3f(p.mPos.mV[VX], p.mPos.mV[VY], p.mGapTop);

        // A tier B bundle's probe gets a halo ring's worth of extra spokes: analysed
        // reverb exists for this one, not just the Sabine estimate.
        if (p.mHaveBundle)
        {
            const F32 d1 = s * 1.6f;
            gGL.color4f(c.mV[0], c.mV[1], c.mV[2], 0.45f);
            gGL.vertex3f(p.mPos.mV[VX] - d1, p.mPos.mV[VY], p.mPos.mV[VZ]);
            gGL.vertex3f(p.mPos.mV[VX] + d1, p.mPos.mV[VY], p.mPos.mV[VZ]);
            gGL.vertex3f(p.mPos.mV[VX], p.mPos.mV[VY] - d1, p.mPos.mV[VZ]);
            gGL.vertex3f(p.mPos.mV[VX], p.mPos.mV[VY] + d1, p.mPos.mV[VZ]);
        }
    }

    // The listener's blend: the connected probes it is actually interpolating
    // between, line alpha by weight - the visual answer to "what is the room verdict
    // standing on".
    for (S32 idx : fd.mListenerProbes)
    {
        if (idx < 0 || idx >= (S32)fd.mProbes.size()) continue;
        const SSWorldField::AcousticDebug::Probe& p = fd.mProbes[(size_t)idx];
        const F32 dist = dist_vec(p.mPos, cam);
        const F32 a = llclamp(BLEND_LINE.mV[3] * 8.f / llmax(dist, 1.f), 0.15f, BLEND_LINE.mV[3]);
        gGL.color4f(BLEND_LINE.mV[0], BLEND_LINE.mV[1], BLEND_LINE.mV[2], a);
        gGL.vertex3fv(cam.mV);
        gGL.vertex3fv(p.mPos.mV);
    }

    // The last thunder's occlusion story: the direct line the euclidean delay
    // assumes - green when the span store says the line is clear, red where it
    // crosses solid - against the propagated path the graph actually routes, with
    // each portal hop crossed.
    if (d.mThunder.mValid && d.mThunderAge <= 30.0)
    {
        const bool occluded = d.mThunder.mCrossings > 0;
        gGL.color4fv((occluded ? PATH_OCCL : PATH_CLEAR).mV);
        gGL.vertex3fv(cam.mV);
        gGL.vertex3fv(d.mThunder.mSource.mV);

        if (d.mThunder.mPropagated && d.mThunder.mPath.size() >= 2)
        {
            gGL.color4fv(PROP_PATH.mV);
            for (size_t i = 1; i < d.mThunder.mPath.size(); ++i)
            {
                gGL.vertex3fv(d.mThunder.mPath[i - 1].mV);
                gGL.vertex3fv(d.mThunder.mPath[i].mV);
            }
        }
    }

    gGL.end();

    // Portal hops along the propagated path, as crosses over the lines.
    if (d.mThunder.mValid && d.mThunder.mPropagated && d.mThunder.mPath.size() >= 2)
    {
        // The path is stride-sampled, so the hops are approximated by direction
        // changes between consecutive samples: a portal sits wherever the path's
        // heading breaks. Mark those, dimly, rather than pretending to the exact
        // node the stride skipped.
        gGL.begin(LLRender::LINES);
        for (size_t i = 1; i + 1 < d.mThunder.mPath.size(); ++i)
        {
            const LLVector3 in = d.mThunder.mPath[i] - d.mThunder.mPath[i - 1];
            const LLVector3 out = d.mThunder.mPath[i + 1] - d.mThunder.mPath[i];
            if (in.magVecSquared() < 1.0e-4f || out.magVecSquared() < 1.0e-4f) continue;
            const F32 dot = in * out / (in.magVec() * out.magVec());
            if (dot > 0.999f) continue;   // straight through: no hop marked

            const F32 s = 1.5f;
            gGL.color4fv(PORTAL_HOP.mV);
            gGL.vertex3f(d.mThunder.mPath[i].mV[VX] - s, d.mThunder.mPath[i].mV[VY], d.mThunder.mPath[i].mV[VZ]);
            gGL.vertex3f(d.mThunder.mPath[i].mV[VX] + s, d.mThunder.mPath[i].mV[VY], d.mThunder.mPath[i].mV[VZ]);
            gGL.vertex3f(d.mThunder.mPath[i].mV[VX], d.mThunder.mPath[i].mV[VY] - s, d.mThunder.mPath[i].mV[VZ]);
            gGL.vertex3f(d.mThunder.mPath[i].mV[VX], d.mThunder.mPath[i].mV[VY] + s, d.mThunder.mPath[i].mV[VZ]);
        }
        gGL.end();
    }

    // Labels: the listener's blended RT60 verdict where the camera stands, and the
    // last thunder's figures at its source.
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    if (font)
    {
        if (d.mThunder.mValid && d.mThunderAge <= 30.0)
        {
            std::string label;
            if (d.mThunder.mPropagated)
            {
                label = llformat("path %.0f m (+%.2f s)  direct %.0f m  portals %d  muffle %.2f",
                                 d.mThunder.mPathM, d.mThunder.mDelayS, d.mThunder.mDirectM,
                                 d.mThunder.mPortals,
                                 llclamp(1.f - (1.f - d.mThunder.mMuffleCloud) * (1.f - d.mThunder.mMuffleField), 0.f, 1.f));
            }
            else
            {
                label = llformat("direct %.0f m  no propagation (no bake or no path)", d.mThunder.mDirectM);
            }
            hud_render_utf8text(label, d.mThunder.mSource, *font, LLFontGL::NORMAL,
                                LLFontGL::DROP_SHADOW, 6.f, 4.f, PROP_PATH, false);
        }

        // The tile's own summary, a little above the camera.
        const std::string summary = llformat("%d probes  %d links  %d portals%s",
                                             d.mProbeCount, d.mLinkCount, d.mPortalCount,
                                             d.mQuality >= 1 ? llformat("  tier B %d/%d", d.mBundleCount, d.mProbeCount).c_str() : "");
        hud_render_utf8text(summary, cam + LLVector3(0.f, 0.f, 6.f), *font, LLFontGL::NORMAL,
                            LLFontGL::DROP_SHADOW, 6.f, 4.f, TEXT_NORMAL, false);
    }
}

// ---------------------------------------------------------------------------
// The in-world layer: V9's lightning
// ---------------------------------------------------------------------------

// <SS:Nexii> The V9 layer (doc/atmo_magic_debug_views.md V9): every LIVE strike drawn as the geometry the model
// actually holds, plus the lifecycle state it is in and the two lights it feeds. Per strike: the CHANNEL, one
// line per node-to-parent segment straight off SSStrike::mChannel (never re-derived - the channel is rolled once
// at spawn from the fire time and this view only reads it), coloured by the strike's lifecycle stage
// (SSAtmoInfoViewCore::strikeStage / strikeStageColor - the same cool-to-warm bar V2's cells use) and split by
// the LEADER FRONT: the stretch the leader has reached draws solid, the stretch ahead of it draws faint, so a
// bolt mid-descent shows exactly how far down it is; the surface CRAWL run past the foot in its own amber; the
// ATTACHMENT as a cross; a pending strike's aim line from origin to intended ground, which is all a sheet strike
// (no channel at all) ever has; the ground show's own bounding box in grey when the renderer's occlusion query
// says it is hidden - the answer to "why is there no flash on my screen"; and the two LIGHT readings, the
// deferred point lights SSLightning::sceneLights() exports (drawn at their true radius, which is what the world
// is actually lit by) and a mark on every strike whose brightness clears the cloud shader's own cut. Distance
// LOD: far channels reduce to their trunk through SSAtmoInfoViewCore::channelWidthCutoff, and every ring is
// segment-thinned by ringSegments, so a storm's worth of bolts stays a handful of lines. Squash-corrected
// through the cloud field exactly like the bolts themselves are (sslightningrender.cpp applies the same
// squashScale), so the diagram lands ON the bolt rather than behind it. TILE TINT: the ground-fire discs and the
// filled light radii are diagrammatic FILL and sit behind SSAtmoInfoViewTileTint, per the V2/V3 precedent; every
// entity mark - channel, crawl, attachment, rings, labels - always draws. [interaction: SSLightning strikes/sceneLights] [interaction: SSVolCloud squashScale]
void SSAtmoInfoView::renderLightning()
{
    if (!SSLightning::instanceExists()) return;

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    if (!camera) return;
    const LLVector3 cam = camera->getOrigin();

    const LightningData d = lightningData();
    const std::vector<SSStrike>& strikes = SSLightning::getInstance()->strikes();
    // The row table was gathered from this same list, this same frame, in this same order - the loops below index
    // the two together and this is the assertion of that, not a resize.
    if (d.mStrikes.size() != strikes.size()) return;

    const SSVolCloud* clouds = SSVolCloud::instanceExists() ? SSVolCloud::getInstance() : nullptr;
    auto drawn = [&](const LLVector3& p) -> LLVector3
    {
        if (!clouds) return p;
        const LLVector3 rel = p - cam;
        const F32 dist = rel.magVec();
        if (dist <= 1.0e-4f) return p;
        return cam + rel * clouds->squashScale(dist);
    };

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_FALSE, GL_FALSE); // <SS:Nexii> off, not just no-write: this draws in the 3-D pass after the look pass, on top like a Skylines layer, so it must never be occluded by world geometry
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    // A straight world-space line, subdivided so the far squash bends it rather than shearing it - the same
    // idiom renderVirga's own line lambda uses.
    auto line = [&](const LLVector3& a, const LLVector3& b)
    {
        const LLVector3 span = b - a;
        const S32 segs = llclamp((S32)(span.magVec() / 200.f), 1, 24);
        LLVector3 prev = drawn(a);
        for (S32 i = 1; i <= segs; ++i)
        {
            const LLVector3 next = drawn(a + span * ((F32)i / (F32)segs));
            gGL.vertex3fv(prev.mV);
            gGL.vertex3fv(next.mV);
            prev = next;
        }
    };
    // A horizontal ring about a world point, segment-thinned by its own apparent size (SSAtmoInfoViewCore::
    // ringSegments) - unlike V3/V4's camera-centred rails, these rings sit out at the strike, so the apparent-size
    // heuristic is exactly the right one here.
    auto ring = [&](const LLVector3& centre, F32 radius, const LLColor4& color)
    {
        if (radius <= 0.f) return;
        const S32 n = ringSegments(radius, (centre - cam).magVec());
        gGL.color4fv(color.mV);
        LLVector3 prev = drawn(LLVector3(centre.mV[VX] + radius, centre.mV[VY], centre.mV[VZ]));
        for (S32 i = 1; i <= n; ++i)
        {
            const F32 a = (F32)i / (F32)n * TWO_PI_F;
            const LLVector3 next = drawn(LLVector3(centre.mV[VX] + std::cos(a) * radius,
                                                   centre.mV[VY] + std::sin(a) * radius, centre.mV[VZ]));
            gGL.vertex3fv(prev.mV);
            gGL.vertex3fv(next.mV);
            prev = next;
        }
    };

    // ---- the diagrammatic FILL, behind the tile-tint switch (V2/V3 precedent): the ground fire's own blob discs
    // and the deferred lights' radii as flat discs on their own plane. Heavy, and it buries the channels it sits
    // under, which is exactly what that switch is for.
    static LLCachedControl<bool> tile_tint(gSavedSettings, "SSAtmoInfoViewTileTint", false);
    if (tile_tint)
    {
        gGL.begin(LLRender::TRIANGLES);
        auto disc = [&](const LLVector3& centre, F32 radius, const LLColor4& color)
        {
            if (radius <= 0.f) return;
            const S32 n = ringSegments(radius, (centre - cam).magVec());
            gGL.color4fv(color.mV);
            for (S32 i = 0; i < n; ++i)
            {
                const F32 a0 = (F32)i / (F32)n * TWO_PI_F;
                const F32 a1 = (F32)(i + 1) / (F32)n * TWO_PI_F;
                const LLVector3 c = drawn(centre);
                const LLVector3 p0 = drawn(LLVector3(centre.mV[VX] + std::cos(a0) * radius,
                                                     centre.mV[VY] + std::sin(a0) * radius, centre.mV[VZ]));
                const LLVector3 p1 = drawn(LLVector3(centre.mV[VX] + std::cos(a1) * radius,
                                                     centre.mV[VY] + std::sin(a1) * radius, centre.mV[VZ]));
                gGL.vertex3fv(c.mV); gGL.vertex3fv(p0.mV); gGL.vertex3fv(p1.mV);
            }
        };
        for (const SSStrike& s : strikes)
        {
            if (s.mFire <= 0.f) continue;
            for (const SSStrikeFire& blob : s.mFireBlobs)
            {
                disc(blob.mPos, blob.mRadius, LLColor4(STRIKE_FIRE_FILL.mV[0], STRIKE_FIRE_FILL.mV[1],
                                                       STRIKE_FIRE_FILL.mV[2], STRIKE_FIRE_FILL.mV[3] * llclamp(s.mFire, 0.f, 1.f)));
            }
        }
        for (const LightningData::SceneLight& light : d.mLights)
        {
            disc(light.mPos, light.mRadiusM, LLColor4(STRIKE_SCENELIGHT.mV[0], STRIKE_SCENELIGHT.mV[1],
                                                      STRIKE_SCENELIGHT.mV[2], 0.10f));
        }
        gGL.end();
    }

    // ---- the entity marks: always drawn.
    gGL.begin(LLRender::LINES);
    for (size_t si = 0; si < strikes.size(); ++si)
    {
        const SSStrike& s = strikes[si];
        const LightningData::Strike& row = d.mStrikes[si];   // same list, same order, gathered this frame
        const F32 dist = (s.mOrigin - cam).magVec();
        const F32 cutoff = channelWidthCutoff(dist);
        const LLColor4 stage_col = toColor(strikeStageColor(row.mStage), 1.f);

        // The channel, split at the leader front. A node's reach is its own path distance from the root
        // (SSLightning::finishChannel), so "reached" is exactly the test the renderer itself makes.
        for (size_t i = 0; i < s.mChannel.size(); ++i)
        {
            const SSStrikeNode& node = s.mChannel[i];
            if (node.mParent < 0 || node.mParent >= (S32)s.mChannel.size()) continue;
            if (!node.mTrunk && !node.mCrawl && node.mWidth < cutoff) continue;   // far: trunk (and crawl) only
            const bool reached = node.mReachedAt <= s.mLeaderProgress;
            if (node.mCrawl)
            {
                gGL.color4fv(LLColor4(STRIKE_CRAWL.mV[0], STRIKE_CRAWL.mV[1], STRIKE_CRAWL.mV[2],
                                      reached ? 0.90f : 0.25f).mV);
            }
            else
            {
                gGL.color4fv(LLColor4(stage_col.mV[0], stage_col.mV[1], stage_col.mV[2],
                                      reached ? 0.85f : 0.20f).mV);
            }
            line(s.mChannel[(size_t)node.mParent].mPos, node.mPos);
        }

        // A strike with no channel at all - a sheet flash - still has an origin and an intended ground point, and
        // so does any strike still counting down; the aim line is the only geometry either has to show.
        if (s.mChannel.empty())
        {
            gGL.color4fv(STRIKE_AIM.mV);
            line(s.mOrigin, s.mGround);
        }

        // The attachment: a cross at the ground point, sized so it reads from the air but never swamps the crawl.
        if (row.mKind == STRIKE_KIND_GROUND)
        {
            const F32 arm = llclamp(dist * 0.01f, 2.f, 25.f);
            gGL.color4fv(row.mOccHidden ? STRIKE_OCCLUDED.mV : STRIKE_ATTACH.mV);
            line(s.mGround - LLVector3(arm, 0.f, 0.f), s.mGround + LLVector3(arm, 0.f, 0.f));
            line(s.mGround - LLVector3(0.f, arm, 0.f), s.mGround + LLVector3(0.f, arm, 0.f));
            line(s.mGround, s.mGround + LLVector3(0.f, 0.f, arm * 2.f));

            // Hidden by the renderer's own occlusion query: outline the ground box that query was issued on, so
            // "the show is running but nothing is on screen" is answerable without guessing.
            if (row.mOccHidden)
            {
                const LLVector3& lo = s.mGroundBoxMin;
                const LLVector3& hi = s.mGroundBoxMax;
                gGL.color4fv(STRIKE_OCCLUDED.mV);
                line(LLVector3(lo.mV[VX], lo.mV[VY], lo.mV[VZ]), LLVector3(hi.mV[VX], lo.mV[VY], lo.mV[VZ]));
                line(LLVector3(hi.mV[VX], lo.mV[VY], lo.mV[VZ]), LLVector3(hi.mV[VX], hi.mV[VY], lo.mV[VZ]));
                line(LLVector3(hi.mV[VX], hi.mV[VY], lo.mV[VZ]), LLVector3(lo.mV[VX], hi.mV[VY], lo.mV[VZ]));
                line(LLVector3(lo.mV[VX], hi.mV[VY], lo.mV[VZ]), LLVector3(lo.mV[VX], lo.mV[VY], lo.mV[VZ]));
            }
        }

        // The cloud-light reading: a small ring at the origin on every strike the deck's own shader is lit by.
        if (row.mLightsCloud)
        {
            ring(s.mOrigin, llmax(dist * 0.02f, 12.f), STRIKE_CLOUDLIGHT);
        }
    }

    // The deferred scene lights at their true radius - the set the world is actually lit by this frame.
    for (const LightningData::SceneLight& light : d.mLights)
    {
        ring(light.mPos, light.mRadiusM, STRIKE_SCENELIGHT);
    }
    gGL.end();

    // ---- labels: one per strike, at its origin, plus the radius on each scene light.
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    if (!font) return;

    for (size_t si = 0; si < strikes.size(); ++si)
    {
        const SSStrike& s = strikes[si];
        const LightningData::Strike& row = d.mStrikes[si];
        const LLColor4 stage_col = toColor(strikeStageColor(row.mStage), 1.f);
        // Before contact the clock is a countdown; after it, an age. Both are the same mT, which is why they are
        // printed from one field with one sign test rather than two clocks.
        const std::string when = (row.mT < 0.f) ? llformat("in %.2fs", -row.mT) : llformat("+%.2fs", row.mT);
        hud_render_utf8text(llformat("%s %s  %s  I %.2f  %s%s%s%s", strikeKindLabel(row.mKind),
                                     strikeStageLabel(row.mStage), when.c_str(), row.mIntensity,
                                     row.mPositive ? "positive" : "negative",
                                     row.mBlue ? "  BLUE" : "", row.mForced ? "  FORCED" : "",
                                     row.mOccHidden ? "  occluded" : ""),
                            drawn(s.mOrigin), *font, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, 6.f, 4.f,
                            stage_col, false);

        // The second line carries the numbers the legend cannot show per strike: how much of the channel the
        // leader has, how many return strokes have fired, and what this strike is worth as a light.
        hud_render_utf8text(llformat("leader %.0f%%  strokes %d  nodes %d  light %.3f%s  %.0f m",
                                     llclamp(row.mLeaderProgress, 0.f, 1.f) * 100.f, row.mStrokeCount,
                                     row.mChannelNodes, row.mLightWeight, row.mLightsCloud ? " (deck lit)" : "",
                                     row.mDistanceM),
                            drawn(s.mOrigin + LLVector3(0.f, 0.f, -llclamp((s.mOrigin - cam).magVec() * 0.01f, 4.f, 40.f))),
                            *font, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, 6.f, 4.f, TEXT_DIM, false);
    }

    for (const LightningData::SceneLight& light : d.mLights)
    {
        hud_render_utf8text(llformat("scene light  r %.0f m", light.mRadiusM), drawn(light.mPos), *font,
                            LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, 6.f, 4.f, STRIKE_SCENELIGHT, false);
    }
}

// ---------------------------------------------------------------------------
// SSAtmoDimView
// ---------------------------------------------------------------------------

SSAtmoDimView::SSAtmoDimView(const Params& p)
:   LLView(p)
{
}

// <SS:Nexii> Draws NOTHING, on purpose. It briefly drew the dim quad and then re-entered 3-D from here to put the in-world layer on top of it; that put a 3-D layer inside the UI stage, which broke the UI around it - clipped console text, visualisations flung into the window corners. Both halves now draw from the 3-D pass instead (SSAtmoInfoView::renderDimAndWorld, called by render_ui() in llviewerdisplay.cpp - post-tonemap, after renderFinalize and before render_hud_attachments), and the UI stage keeps only what is genuinely 2-D: the legend and the chart. The view itself stays registered and attached so LLDebugView's addChildInBack and the debug view's child order do not move - deleting it would reshuffle every sibling's z-order for no gain. Do not put the quad back here.
void SSAtmoDimView::draw()
{
}

// ---------------------------------------------------------------------------
// SSAtmoLegendView
// ---------------------------------------------------------------------------

SSAtmoLegendView::SSAtmoLegendView(const Params& p)
:   LLView(p)
{
}

void SSAtmoLegendView::buildWindProfileSpec(Spec& spec)
{
    const SSAtmoInfoView::WindProfileData d = SSAtmoInfoView::windProfileData();
    spec.mTitle = "V1  WIND PROFILE";
    spec.mSubtitle = d.mValid
        ? llformat("10m %.1f m/s @ %03.0f  S %.2f  veer %.0f  exp %.2f", d.mParams.mSpeed10MS, d.mParams.mHeading10Deg,
                   d.mParams.mShearStrength, d.mParams.mVeerDeg, d.mParams.mExponent)
        : "no weather cube applied";
    spec.mRampLabel = "wind speed (arrow colour, curve)";
    spec.mRampMin = "0 m/s";
    spec.mRampMax = llformat("%.0f m/s", d.mMaxSpeed);
    spec.mRamp = &rampSpeed;
    spec.mKeys.push_back({ RAIL_REF,    "10 m reference wind", true });
    spec.mKeys.push_back({ RAIL_BL,     "1500 m boundary-layer top", true });
    spec.mKeys.push_back({ RAIL_BASE,   d.mDeckBuilt ? llformat("deck base  z %.0f", d.mBaseZ) : "deck base  (not built)", true });
    spec.mKeys.push_back({ RAIL_LID,    d.mDeckBuilt ? llformat("deck lid   z %.0f", d.mLidZ) : "deck lid   (not built)", true });
    spec.mKeys.push_back({ RAIL_CIRRUS, llformat("cirrus band now  z %.0f", d.mCirrusZ), true });
    spec.mKeys.push_back({ CURVE_LIVE,  "live: weather-cube exponent", true });
    spec.mKeys.push_back({ CURVE_FLOW,  llformat("flowmap region exp %.2f%s", d.mFlowAlpha, d.mFlowSolved ? "" : " (fallback)"), true });
    spec.mKeys.push_back({ CURVE_GRAD,  llformat("gradient setting %.2f", d.mGradientSetting), true });
    spec.mKeys.push_back({ TEXT_DIM,    "mast arrows point the way the air moves", false });
}

// V2's legend: the potential ramp the tiles are tinted with (threshold named as the floor), a row per lifecycle stage in ring colour, the hero ribbon and anchor marks, the two rotation hues - and, with no hero while an Allow checkbox is on, the scheduler's "why not" terms with their live values (SSStormCells::WhyNot), so formation can be debugged without reading code.
void SSAtmoLegendView::buildStormCellsSpec(Spec& spec)
{
    const SSAtmoInfoView::StormCellsData d = SSAtmoInfoView::stormCellsData();
    spec.mTitle = "V2  STORM CELLS";
    if (!d.mValid)
    {
        spec.mSubtitle = "scheduler not running (no track, region or asset)";
        return;
    }
    spec.mSubtitle = llformat("%d alive  %d spawned  %d super  %d eligible  %s  z %.0f%s", d.mAlive, d.mSpawned, d.mSupercells, d.mTornadoEligible,
                              d.mHaveHero ? "HERO LIVE" : "no hero", d.mLayerZ, d.mDeckBuilt ? "" : " (deck not built)");
    static LLCachedControl<bool> tile_tint(gSavedSettings, "SSAtmoInfoViewTileTint", false);
    if (tile_tint)
    {
        spec.mRampLabel = llformat("lattice potential P (tile tint; spawn floor %.2f)", SSStormCell::SPAWN_THRESHOLD);
        spec.mRampMin = "0";
        spec.mRampMax = "1";
        spec.mRamp = &rampEnergy;
    }
    else
    {
        spec.mRampLabel = "(tile tint off)";
    }

    if (tile_tint)
    {
        spec.mKeys.push_back({ TILE_ALIVE, "tile outline bright: an active cell was born there", true });
    }
    static const char* kBands[STAGE_COUNT] = { "0.00-0.15", "0.15-0.30", "0.30-0.55", "0.55-0.80", "0.80-1.00" };
    for (S32 s = 0; s < STAGE_COUNT; ++s)
    {
        spec.mKeys.push_back({ toColor(stageColor(s), 1.f), llformat("ring: %-8s age %s", stageLabel(s), kBands[s]), true });
    }
    spec.mKeys.push_back({ HERO_ORIGIN, "hero origin + motion arrow", true });
    // 7f V2 item 3: age at closest approach (fraction of the hero's own life, HERO_CLOSEST_AGE01 == 0.5 whatever the
    // wind unless the spawn clamp bit) alongside the measured distance, plus the pass law itself in one line - the
    // no-hero fallback names the two rings actually drawn (the law's median and HERO_PASS_MAX_M), not
    // HERO_PASS_MIN_M (0 m, which the ring code skips and draws nothing for).
    const F32 closest_age01 = (d.mHaveHero && d.mHero.mLifetimeS > 0.f)
        ? llclamp((F32)((d.mHero.mClosestTime - d.mHero.mBirthTime) / (F64)d.mHero.mLifetimeS), 0.f, 1.f) : 0.f;
    const F32 pass_median_m = SSStormCell::HERO_PASS_MAX_M * std::pow(0.5f, SSStormCell::HERO_PASS_SKEW);
    spec.mKeys.push_back({ ANCHOR_WHITE, d.mHaveHero
        ? llformat("now diamond; closest approach %.0f m at age %.2f", d.mHero.mClosestDistM, closest_age01)
        : llformat("anchor cross, %.0f / %.0f m pass rings (median / max)", pass_median_m, SSStormCell::HERO_PASS_MAX_M), true });
    spec.mKeys.push_back({ TEXT_DIM, llformat("pass law: %.0f m x u^%.2f, u hashed (median ~300 m, never beyond %.0f m)",
                                              SSStormCell::HERO_PASS_MAX_M, SSStormCell::HERO_PASS_SKEW, SSStormCell::HERO_PASS_MAX_M), false });
    spec.mKeys.push_back({ HERO_DEATH, d.mHaveHero ? llformat("hero death; ribbon ticks every %.0f s", ribbonTickIntervalS(d.mHero.mLifetimeS)) : "hero death", true });
    spec.mKeys.push_back({ toColor(rotationRamp(1.f), 1.f), "rotation + cyclonic: arrows orbit anticlockwise", true });
    spec.mKeys.push_back({ toColor(rotationRamp(-1.f), 1.f), "rotation - anticyclonic: arrows orbit clockwise", true });

    // <SS:Nexii> SQUALL/FORCED (doc/atmo_magic_storm_dynamics.md sections 5-6): the bar through a line's own
    // members, its leading-edge QLCS junction markers, and the forced/authored pin's distinct double outline.
    spec.mKeys.push_back({ SQUALL_LINE, "squall line: bar through a line's own active members", true });
    spec.mKeys.push_back({ SQUALL_JUNCTION, "squall line: leading-edge QLCS junction marker", true });
    spec.mKeys.push_back({ FORCED_OUTLINE, "forced/authored cell: double outline (mStormOverride cue)", true });
    // <SS:Nexii> LINE BAND (doc/atmo_magic_phase8_show.md section 3 item 1): the deck coupling's own band extent -
    // the widths are the fixed design constants (SSSquall::LINE_BAND_M/LINE_SHELF_M, the same ones
    // SSStormCells::fillLineBand writes into bandM/shelfM whenever a line IS alive), named here regardless of
    // whether one is alive this instant; fill alpha at the draw site scales with the line's live strength, so "no
    // fill drawn" and "no line alive" are the same statement.
    spec.mKeys.push_back({ LINE_WALL_FILL, llformat("line wall fill: %.0f m either side of the segment", SSSquall::LINE_BAND_M), false });
    spec.mKeys.push_back({ LINE_SHELF_FILL, llformat("line shelf fill: gust front reaching %.0f m ahead", SSSquall::LINE_SHELF_M), false });

    // <SS:Nexii> DEBUG: vortex icons - one swatch pair (the icon carries the SAME sign-only rotation colour as the
    // cell arrows above it, per vortexKindColor's own comment), then the taxonomy label list an icon can read as
    // (kind is text, not colour - see ssatmoinfoviewcore.h's vortexKindLabel), plus the condensation bar and
    // collar-wireframe/multi-vortex keys.
    spec.mKeys.push_back({ toColor(vortexKindColor(1.f), 1.f), "vortex icon (+): mesocyclonic / landspout / waterspout / satellite", true });
    spec.mKeys.push_back({ toColor(vortexKindColor(-1.f), 1.f), "vortex icon (-): anticyclonic", true });
    spec.mKeys.push_back({ TEXT_DIM, "vortex icon (funnel-less): gustnado", false });
    spec.mKeys.push_back({ DUST_COLOR, "dust devil (tan cross, no funnel)", true });
    spec.mKeys.push_back({ TEXT_DIM, "condensation bar: filled 0 (aloft) -> 1 (touchdown) -> 0 (roped out)", false });
    spec.mKeys.push_back({ TEXT_DIM, "collar rings: the funnel's own radius/altitude table, wireframe", false });
    spec.mKeys.push_back({ TEXT_DIM, "label N: multi-vortex suction-vortex count (absent = single vortex)", false });

    if (!d.mHaveHero)
    {
        if (!d.mAllowSupercells && !d.mAllowTornadoes)
        {
            spec.mKeys.push_back({ TEXT_DIM, "no hero: Allow Supercells / Allow Tornadoes off (Weather Influence)", false });
        }
        else
        {
            spec.mKeys.push_back({ TEXT_DIM, llformat("WHY NOT (best candidate)  Allow super %s  tornado %s", d.mAllowSupercells ? "on" : "OFF",
                                                      d.mAllowTornadoes ? "on" : "OFF"), false });
            for (const std::string& f : d.mWhyNot)
            {
                spec.mKeys.push_back({ TEXT_DIM, "  " + f, false });
            }
        }
    }
    else if (!d.mVortexWhyNot.mAlive)
    {
        // <SS:Nexii> DEBUG: the tornado "why not" readout - the hero DOES exist (the block above's WHY NOT never
        // fires here), but it carries no live tornado-family funnel right now (SSVortices::WhyNotTornado). Distinct
        // question from "why no hero" above it: this is "why not a tornado on the hero we have".
        spec.mKeys.push_back({ TEXT_DIM, llformat("TORNADO WHY NOT (hero)  meso %.2f  potential %.2f  super %s  eligible %s  allowTornadoes %s",
                                                  d.mVortexWhyNot.mMeso, d.mVortexWhyNot.mPotential, d.mVortexWhyNot.mSupercell ? "yes" : "no",
                                                  d.mVortexWhyNot.mTornadoEligible ? "yes" : "no", d.mVortexWhyNot.mAllowTornadoes ? "on" : "OFF"), false });
        for (const std::string& f : d.mVortexWhyNot.mFailing)
        {
            spec.mKeys.push_back({ TEXT_DIM, "  " + f, false });
        }
    }
}

// V3's legend: the five LOD rails in ring colour, the tile ramp (SSDeckLod::keepFrac, floored at THIN_KEEP_MIN),
// and the live puff count against the budget dial plus the LOD-predicted (pre-thin, pre-budget) figure so the
// two can be read against each other - a predicted figure well over the budget means the far thinning is doing
// most of the work; one close to it means the budget clamp (buildDeck's own LL_DEBUGS line) is the backstop
// actually firing.
void SSAtmoLegendView::buildDeckLodSpec(Spec& spec)
{
    const SSAtmoInfoView::DeckLodData d = SSAtmoInfoView::deckLodData();
    spec.mTitle = "V3  DECK LOD";
    if (!d.mDeckBuilt)
    {
        spec.mSubtitle = "no cloud deck built";
        return;
    }
    spec.mSubtitle = llformat("placed %d / budget %d  dial %d  LOD-predicted %lld over %d cells",
                              d.mPuffsPlaced, d.mBudget, d.mPuffsPerCell, (long long)d.mLodPredicted, d.mCellsWalked);
    static LLCachedControl<bool> tile_tint(gSavedSettings, "SSAtmoInfoViewTileTint", false);
    if (tile_tint)
    {
        spec.mRampLabel = llformat("keep fraction (tile tint; floor %.2f)", SSDeckLod::THIN_KEEP_MIN);
        spec.mRampMin = "0";
        spec.mRampMax = "1";
        spec.mRamp = &rampGrey;
    }
    else
    {
        spec.mRampLabel = "(tile tint off)";
    }

    spec.mKeys.push_back({ RING_SUBS_FULL,  llformat("subs full below  %.0f m", SSDeckLod::SUBS_FULL_M), true });
    spec.mKeys.push_back({ RING_SUBS_TWO,   llformat("2 subs until  %.0f m", SSDeckLod::SUBS_TWO_M), true });
    spec.mKeys.push_back({ RING_THIN_START, llformat("far thinning from  %.0f m", SSDeckLod::THIN_START_M), true });
    spec.mKeys.push_back({ RING_FADE_START, llformat("edge fade from  %.0f m", SSDeckLod::FIELD_FADE_START_M), true });
    spec.mKeys.push_back({ RING_DECK_EDGE,  llformat("deck edge  %.0f m", SSDeckLod::DECK_EDGE_M), true });
    // <SS:Nexii> LOD phase 6d (ssdeckmacrocore.h CONTRACT): Tier B's own line - the merged macro-puff bodies
    // placed this build (already counted inside mPuffsPlaced above; broken out here so the crossfade band and the
    // body count can be read against each other), and the tinted macro grid renderDeckLod draws beyond TIER_B_M.
    spec.mKeys.push_back({ RING_TIER_B, llformat("Tier B macro bodies  %d  (merge from  %.0f m, blend  %.0f m)",
                                                  d.mTierBCount, SSDeckMacro::TIER_B_M, SSDeckMacro::TIER_BLEND_M), true });
    if (tile_tint)
    {
        spec.mKeys.push_back({ TEXT_DIM, "tile tint: a distance-only diagram, not the builder's own cell gate", false });
    }
}

// V4's legend: the drive scale (SSVirga::drive, presence ramp) the cell outlines are tinted by, the qualifying
// count against SSVirga::MAX_SHAFTS and how many the hashed trim left out, the fall-tilt line's own colour (the
// deck-base rail's blue, shared with V1), and the handoff ring/band. mActive false reads as "off" rather than
// showing a stale snapshot from a build where Distant Rain was on.
void SSAtmoLegendView::buildVirgaSpec(Spec& spec)
{
    const SSAtmoInfoView::VirgaData d = SSAtmoInfoView::virgaData();
    spec.mTitle = "V4  PRECIP & VIRGA";
    if (!d.mValid)
    {
        spec.mSubtitle = "no weather cube applied";
        return;
    }
    if (!d.mActive)
    {
        spec.mSubtitle = "Distant Rain off (Weather Influence), or this deck is not the storm-coupled one";
        return;
    }
    spec.mSubtitle = llformat("%d qualifying  kept %d / %d  trimmed %d  r2 %.0f m%s", d.mCandidates, d.mKept,
                              SSVirga::MAX_SHAFTS, d.mTrimmed, d.mR2, d.mDeckBuilt ? "" : "  (deck not built)");
    spec.mRampLabel = "shaft drive (precip x presence x tower)";
    spec.mRampMin = "0";
    spec.mRampMax = "1";
    spec.mRamp = &rampGrey;

    spec.mKeys.push_back({ TEXT_NORMAL, "cell outline (at the embedded stack's own top): bright = kept, dim = trimmed by MAX_SHAFTS", true });
    spec.mKeys.push_back({ RAIL_BASE, "fall-tilt: deck base entry -> landing (precip's own ground-wind approximation, kept cells)", true });
    spec.mKeys.push_back({ VIRGA_SKEW, "curtain skew: base -> its OWN skewed ground point (emitter's actual wind/fall, kept cells)", true });
    spec.mKeys.push_back({ VIRGA_HANDOFF, llformat("handoff ring  r2 x %.1f = %.0f m (particle rain sheets)", SSVirga::HANDOFF_SKIP, d.mHandoffRadius), true });
    spec.mKeys.push_back({ LLColor4(VIRGA_HANDOFF.mV[0], VIRGA_HANDOFF.mV[1], VIRGA_HANDOFF.mV[2], 0.25f),
                           llformat("handoff band  +%.0f m to full shaft alpha", SSVirga::HANDOFF_BAND_M), true });
    spec.mKeys.push_back({ TEXT_DIM, llformat("qualify threshold %.2f / Distant Rain strength", SSVirga::THRESHOLD), false });
}

// V5's legend: one swatch per curve, split across the chart's two lanes (top: the authored day-cycle curves;
// bottom: the derived gates), the "now" cursor, and the two cue-marker colours (storm cue vs precipitation cue).
void SSAtmoLegendView::buildWeatherCubeSpec(Spec& spec)
{
    const SSAtmoInfoView::WeatherCubeData d = SSAtmoInfoView::weatherCubeData();
    spec.mTitle = "V5  WEATHER CUBE";
    if (!d.mValid)
    {
        spec.mSubtitle = "no weather cube applied";
        return;
    }
    spec.mSubtitle = llformat("track '%s'  day length %.0f min  %d cues", d.mTrackName.c_str(), d.mDayLengthS / 60.0, (S32)d.mCues.size());

    spec.mKeys.push_back({ CUBE_MOISTURE, "top lane: moisture", true });
    spec.mKeys.push_back({ CUBE_CONVECTION, "top lane: convection", true });
    spec.mKeys.push_back({ CUBE_TEMP, "top lane: temperature C (own scale)", true });
    spec.mKeys.push_back({ CUBE_WIND, "top lane: wind speed (heading in the tooltip readout)", true });
    spec.mKeys.push_back({ CUBE_SHEAR, "top lane: shear strength (veer deg on its own scale)", true });
    spec.mKeys.push_back({ CUBE_CONSOLIDATE, "bottom lane: storm consolidation (SSWindProfile::consolidation)", true });
    spec.mKeys.push_back({ CUBE_GLOOM, "bottom lane: puff gloom (1 = fair-weather albedo, 0 = darkest)", true });
    spec.mKeys.push_back({ CUBE_ANVIL, "bottom lane: anvil ramp (deck-wide)", true });
    spec.mKeys.push_back({ CUBE_LIGHTNING, "bottom lane: lightning gate (intensity, 0 = no strikes)", true });
    spec.mKeys.push_back({ CUBE_STORM_SCORE, "bottom lane: storm spawn score at the anchor's own lattice cell", true });
    spec.mKeys.push_back({ CUBE_NOW, "now cursor (SSAtmoEnvApplier::appliedPhase)", true });
    spec.mKeys.push_back({ CUBE_CUE_STORM, "cue marker: authored mStormOverride keyframe", true });
    spec.mKeys.push_back({ CUBE_CUE_PRECIP, "cue marker: authored mPrecipitationOverride keyframe", true });
}

// V9's legend: the five lifecycle stages in the colour the channels are drawn in, with the live count in each, so
// "three bolts on screen, two of them already in plasma" reads at a glance; the marks that are not a stage colour
// (crawl, attachment, the two light readings, the occlusion grey); the light budgets - how many strikes the cloud
// deck's own shader is lit by against SS_MAX_STRIKE_LIGHTS, and how many deferred point lights the world got
// against the slots pipeline.cpp asks for; the renderer's own last-frame counters; and - the "why is nothing
// striking" readout, the same idea as V2's "why not" rows - the applied weather's lightning gate with its live
// values whenever no strike is alive.
void SSAtmoLegendView::buildLightningSpec(Spec& spec)
{
    const SSAtmoInfoView::LightningData d = SSAtmoInfoView::lightningData();
    spec.mTitle = modeLabel(MODE_LIGHTNING);
    if (!d.mValid)
    {
        spec.mSubtitle = "lightning system not running";
        return;
    }

    const S32 live = (S32)d.mStrikes.size();
    spec.mSubtitle = llformat("%d live  next %s  deck lit %d/%d  scene lights %d/%d", live,
                              (d.mNextIn >= 0.0) ? llformat("in %.1fs", d.mNextIn).c_str() : "not scheduled",
                              d.mCloudLit, d.mCloudCap, (S32)d.mLights.size(), d.mSceneLightCap);

    S32 per_stage[STRIKE_STAGE_COUNT] = { 0 };
    for (const auto& s : d.mStrikes)
    {
        per_stage[llclamp(s.mStage, 0, STRIKE_STAGE_COUNT - 1)] += 1;
    }
    for (S32 i = 0; i < STRIKE_STAGE_COUNT; ++i)
    {
        spec.mKeys.push_back({ toColor(strikeStageColor(i), 1.f),
                               llformat("%s  x%d", strikeStageLabel(i), per_stage[i]), true });
    }
    spec.mKeys.push_back({ TEXT_DIM, "channel: solid past the leader front, faint ahead of it", true });
    spec.mKeys.push_back({ STRIKE_CRAWL, "ground crawl (surface run past the foot)", true });
    spec.mKeys.push_back({ STRIKE_ATTACH, "attachment point", true });
    spec.mKeys.push_back({ STRIKE_AIM, "aim line: origin -> intended ground (sheet strikes have only this)", true });
    spec.mKeys.push_back({ STRIKE_SCENELIGHT, llformat("deferred scene light, drawn at its true radius (%d slots)", d.mSceneLightCap), true });
    spec.mKeys.push_back({ STRIKE_CLOUDLIGHT, llformat("lights the cloud deck: brightness x intensity over %.3f, first %d only",
                                                       STRIKE_LIGHT_CUT, d.mCloudCap), true });
    spec.mKeys.push_back({ STRIKE_OCCLUDED, "ground show hidden by the renderer's own occlusion query", true });

    static LLCachedControl<bool> tile_tint(gSavedSettings, "SSAtmoInfoViewTileTint", false);
    if (tile_tint)
    {
        spec.mKeys.push_back({ STRIKE_FIRE_FILL, "ground-fire blob discs and light radii (tile tint)", false });
    }
    else
    {
        spec.mKeys.push_back({ TEXT_DIM, "(tile tint off: fire discs and filled light radii hidden)", false });
    }

    if (SSLightningRender::instanceExists())
    {
        const SSLightningRender::DrawStats& st = SSLightningRender::getInstance()->stats();
        spec.mKeys.push_back({ TEXT_DIM, llformat("renderer: %d strikes  %d bright  %d segs  %d quads  %d occluded%s",
                                                  st.mStrikes, st.mBright, st.mSegments, st.mQuads, st.mOccluded,
                                                  st.mShaderOk ? "" : "  SHADER NOT READY"), false });
    }

    // The gate, when nothing is alive: every term the scheduler needs, with its live value.
    if (live == 0)
    {
        spec.mKeys.push_back({ d.mEnabled ? TEXT_NORMAL : LLColor4(1.f, 0.3f, 0.3f, 1.f),
                               llformat("why not: lightning %s in the applied weather", d.mEnabled ? "ON" : "OFF"), false });
        spec.mKeys.push_back({ (d.mIntensity > 0.f) ? TEXT_NORMAL : LLColor4(1.f, 0.3f, 0.3f, 1.f),
                               llformat("why not: intensity %.2f", d.mIntensity), false });
        spec.mKeys.push_back({ TEXT_DIM, llformat("interval %.0f-%.0f s  charge %s  sparks %s", d.mIntervalMinS,
                                                  d.mIntervalMaxS, d.mChargeOn ? "on" : "off",
                                                  d.mSparksOn ? "on" : "off"), false });
    }
}

void SSAtmoLegendView::buildAcousticsSpec(Spec& spec)
{
    const SSAtmoInfoView::AcousticsData d = SSAtmoInfoView::acousticsData();
    spec.mTitle = modeLabel(MODE_ACOUSTICS);

    if (!d.mEnabled)
    {
        spec.mSubtitle = "acoustics off (SSWorldFieldAcoustics)";
        spec.mKeys.push_back({ LLColor4(1.f, 0.3f, 0.3f, 1.f),
                               "why not: the acoustic bake switch is off", false });
        return;
    }
    if (!d.mValid)
    {
        spec.mSubtitle = "no current probe bake";
        spec.mKeys.push_back({ LLColor4(1.f, 0.3f, 0.3f, 1.f),
                               "why not: no world field tile yet, edited (flood pending), or no weather", false });
        spec.mKeys.push_back({ TEXT_DIM,
                               "the bake rides the air flood; hold still and it lands", false });
        return;
    }

    spec.mSubtitle = llformat("%d probes @%.0f m  %d links  %d portals%s",
                              d.mProbeCount, d.mLatCell, d.mLinkCount, d.mPortalCount,
                              d.mQuality >= 1 ? llformat("  tier B %d/%d", d.mBundleCount, d.mProbeCount).c_str() : "");

    // The ramp: baked RT60 on the energy ramp's 0..3 s band - the same ramp a
    // marker's colour would take if this view coloured by decay, kept for the
    // "how dry is the average probe here" reading.
    spec.mRampLabel = "baked RT60 (Sabine, tier A; decay curve, tier B)";
    spec.mRamp = rampEnergy;
    spec.mRampMin = "0 s";
    spec.mRampMax = "3 s";

    // Probe key: one row per air state the bake stores, counting the drawn set.
    S32 per_label[5] = { 0, 0, 0, 0, 0 };
    for (const SSWorldField::AcousticDebug::Probe& p : d.mField.mProbes)
    {
        per_label[llclamp((S32)p.mLabel, 0, 4)] += 1;
    }
    static const S32 label_order[4] = { 1, 2, 3, 4 };
    static const char* label_names[5] = { "solid", "outdoors", "sheltered", "interior", "unknown" };
    for (const S32 l : label_order)
    {
        if (per_label[l] == 0) continue;
        spec.mKeys.push_back({ toColor(airLabelColor(l), 1.f),
                               llformat("%s probe  x%d", label_names[l], per_label[l]), false });
    }

    spec.mKeys.push_back({ LLColor4(0.55f, 0.60f, 0.70f, 0.6f), "graph link (validated by a span-store ray)", true });
    spec.mKeys.push_back({ LLColor4(1.00f, 0.85f, 0.30f, 0.9f), "portal link: crosses the outdoors boundary", true });
    spec.mKeys.push_back({ LLColor4(0.40f, 0.70f, 1.00f, 0.9f), "listener blend: the probes being interpolated, alpha by weight", true });
    spec.mKeys.push_back({ LLColor4(0.35f, 1.00f, 0.45f, 0.8f), "last thunder: direct line, clear through the span store", true });
    spec.mKeys.push_back({ LLColor4(1.00f, 0.25f, 0.20f, 0.9f), "last thunder: direct line crosses solid (occluded)", true });
    spec.mKeys.push_back({ LLColor4(0.20f, 0.95f, 1.00f, 0.95f), "propagated path: the graph's route, delay from its metres", true });
    spec.mKeys.push_back({ TEXT_DIM, "tier B spokes: probes carrying the stochastic bundle", true });

    if (!d.mThunder.mValid)
    {
        spec.mKeys.push_back({ TEXT_DIM, "no thunder scheduled yet - the path lines draw after the first strike", false });
    }
    else if (d.mThunderAge > 30.0)
    {
        spec.mKeys.push_back({ TEXT_DIM, llformat("last thunder %.0f s ago (drawn 30 s)", d.mThunderAge), false });
    }
}

void SSAtmoLegendView::draw()
{
    const U32 mode = SSAtmoInfoView::mode();
    if (mode == MODE_OFF) return;

    Spec spec;
    switch (mode)
    {
        case MODE_WIND_PROFILE: buildWindProfileSpec(spec); break;
        case MODE_STORM_CELLS:  buildStormCellsSpec(spec); break;
        case MODE_DECK_LOD:     buildDeckLodSpec(spec); break;
        case MODE_PRECIP_VIRGA: buildVirgaSpec(spec); break;
        case MODE_WEATHER_CUBE: buildWeatherCubeSpec(spec); break;
        case MODE_LIGHTNING:    buildLightningSpec(spec); break;
        case MODE_ACOUSTICS:    buildAcousticsSpec(spec); break;
        default:
            // <SS:Nexii> The reserved numbers (V6 World Field, V8 Anatomy) and anything past the table land here.
            // modeLabel names them rather than printing a bare integer, so a picker entry added before its spec
            // builder exists says WHICH view is missing instead of "INFO VIEW 8".
            spec.mTitle = modeLabel(mode);
            spec.mSubtitle = "not implemented yet";
            break;
    }

    LLFontGL* font = LLFontGL::getFontMonospace();
    if (!font) return;

    const S32 lh = font->getLineHeight();
    const S32 bar_w = 180;
    const S32 bar_h = 10;
    const S32 swatch = 10;

    // Measure.
    S32 widest = llmax(font->getWidth(spec.mTitle), font->getWidth(spec.mSubtitle));
    S32 needed_h = PAD * 2 + lh * 2;
    if (!spec.mRampLabel.empty())
    {
        // <SS:Nexii> When SSAtmoInfoViewTileTint is off, buildStormCellsSpec/buildDeckLodSpec hand us a label
        // ("(tile tint off)") with mRamp left null - this row then prints that one line instead of the gradient
        // bar and its min/max, so the legend still names the row without drawing a ramp for a tile grid the world
        // layer no longer paints.
        widest = llmax(widest, font->getWidth(spec.mRampLabel));
        if (spec.mRamp)
        {
            widest = llmax(widest, bar_w);
            needed_h += lh + bar_h + 2 + lh + 4;
        }
        else
        {
            needed_h += lh + 4;
        }
    }
    for (const KeyRow& k : spec.mKeys)
    {
        widest = llmax(widest, swatch + 6 + font->getWidth(k.mLabel));
        needed_h += lh;
    }
    const S32 needed_w = widest + PAD * 2;

    const LLRect drawn = getRect();
    const S32 box_top = needed_h; // anchored at the bottom, grows upward

    gl_rect_2d(0, box_top, needed_w, 0, LLColor4(0.f, 0.f, 0.f, 0.55f));

    S32 y = box_top - PAD;
    font->renderUTF8(spec.mTitle, 0, PAD, y, TEXT_NORMAL, LLFontGL::LEFT, LLFontGL::TOP);
    y -= lh;
    font->renderUTF8(spec.mSubtitle, 0, PAD, y, TEXT_DIM, LLFontGL::LEFT, LLFontGL::TOP);
    y -= lh;

    if (!spec.mRampLabel.empty())
    {
        y -= 2;
        font->renderUTF8(spec.mRampLabel, 0, PAD, y, TEXT_DIM, LLFontGL::LEFT, LLFontGL::TOP);
        y -= lh;
        if (spec.mRamp)
        {
            const S32 strips = 32;
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            for (S32 i = 0; i < strips; ++i)
            {
                const S32 x0 = PAD + (bar_w * i) / strips;
                const S32 x1 = PAD + (bar_w * (i + 1)) / strips;
                gl_rect_2d(x0, y, x1, y - bar_h, spec.mRamp((F32)i / (F32)(strips - 1)));
            }
            y -= bar_h + 2;
            font->renderUTF8(spec.mRampMin, 0, PAD, y, TEXT_DIM, LLFontGL::LEFT, LLFontGL::TOP);
            font->renderUTF8(spec.mRampMax, 0, PAD + bar_w, y, TEXT_DIM, LLFontGL::RIGHT, LLFontGL::TOP);
            y -= lh + 2;
        }
        else
        {
            y -= 2;
        }
    }

    for (const KeyRow& k : spec.mKeys)
    {
        const S32 sy = y - (lh - swatch) / 2;
        if (k.mSwatchIsLine)
        {
            gl_line_2d(PAD, sy - swatch / 2, PAD + swatch, sy - swatch / 2, k.mColor);
        }
        else
        {
            gl_rect_2d(PAD, sy, PAD + swatch, sy - swatch, k.mColor);
        }
        font->renderUTF8(k.mLabel, 0, PAD + swatch + 6, y, TEXT_NORMAL, LLFontGL::LEFT, LLFontGL::TOP);
        y -= lh;
    }

    // Sized to content, growing up and right from the bottom-left corner the debug view docked it at.
    if (drawn.getWidth() != needed_w || drawn.getHeight() != needed_h)
    {
        LLRect r = drawn;
        r.mRight = r.mLeft + needed_w;
        r.mTop = r.mBottom + needed_h;
        setRect(r);
    }
}

// ---------------------------------------------------------------------------
// SSAtmoGraphView
// ---------------------------------------------------------------------------

SSAtmoGraphView::SSAtmoGraphView(const Params& p)
:   LLView(p)
{
}

void SSAtmoGraphView::polyline(const std::vector<std::pair<S32, S32> >& pts, const LLColor4& color, bool dashed)
{
    if (pts.size() < 2) return;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.color4fv(color.mV);
    if (dashed)
    {
        gGL.begin(LLRender::LINES);
        for (size_t i = 0; i + 1 < pts.size(); i += 2)
        {
            gGL.vertex2i(pts[i].first, pts[i].second);
            gGL.vertex2i(pts[i + 1].first, pts[i + 1].second);
        }
        gGL.end();
        return;
    }
    gGL.begin(LLRender::LINE_STRIP);
    for (const auto& p : pts)
    {
        gGL.vertex2i(p.first, p.second);
    }
    gGL.end();
}

void SSAtmoGraphView::text(const std::string& s, S32 x, S32 y, const LLColor4& color, bool right_align)
{
    LLFontGL* font = LLFontGL::getFontMonospace();
    if (!font) return;
    font->renderUTF8(s, 0, x, y, color, right_align ? LLFontGL::RIGHT : LLFontGL::LEFT, LLFontGL::TOP);
}

void SSAtmoGraphView::drawMessage(const std::string& msg)
{
    const LLRect r = getLocalRect();
    text(msg, PAD, r.getHeight() - PAD, TEXT_DIM);
}

// <SS:Nexii> CHART (design section 6.2): floating over the world on the debug HUD now, rather than sitting inside
// the Views panel's own tab, so there is no "pick a view above" placeholder to show any more when off, and no
// "no chart yet" placeholder box to float uselessly over the world for a mode the dispatch below does not handle
// (today: only the RESERVED numbers, V6 World Field and V8 Anatomy - V4 and V9 both have their own panels now)
// - both cases draw nothing at all, checked against this SAME switch (not a separately maintained mode list) so a
// future mode gaining a drawX() case picks up a chart automatically.
void SSAtmoGraphView::draw()
{
    const U32 mode = SSAtmoInfoView::mode();
    if (mode == MODE_OFF)
    {
        return;
    }

    // Docked to the legend's right edge, bottom-aligned; re-read every frame since the legend's own width tracks
    // its content (SSAtmoLegendView::draw) and can change from one frame to the next as the active spec's row
    // count changes. FOLLOWS_BOTTOM|FOLLOWS_LEFT (set once in SSAtmoInfoView::attach) keeps this pair anchored
    // together through a window resize the same way the legend anchors itself; this only chases the legend's own
    // content-driven width changes, which a follows flag cannot express. [interaction: SSAtmoLegendView]
    if (LLView* legend = sLegendHandle.get())
    {
        const LLRect legend_rect = legend->getRect();
        const LLRect cur = getRect();
        const S32 want_left = legend_rect.mRight + CHART_GAP;
        const S32 want_bottom = legend_rect.mBottom;
        if (cur.mLeft != want_left || cur.mBottom != want_bottom)
        {
            LLRect r = cur;
            r.mLeft = want_left;
            r.mRight = want_left + CHART_W;
            r.mBottom = want_bottom;
            r.mTop = want_bottom + CHART_H;
            setRect(r);
        }
    }

    switch (mode)
    {
        case MODE_WIND_PROFILE:
        case MODE_STORM_CELLS:
        case MODE_DECK_LOD:
        case MODE_PRECIP_VIRGA:
        case MODE_WEATHER_CUBE:
        case MODE_LIGHTNING:
            break;
        default:
            // <SS:Nexii> Every BUILT view now has a chart (V4's landed with backlog item 5.3, V9's with the
            // lightning view), so what falls through here is the reserved numbers - V6 World Field and V8
            // Anatomy - and nothing else: they draw nothing at all, not even the background quad, rather than
            // floating an empty box over the world.
            return;
    }

    const LLRect r = getLocalRect();
    gl_rect_2d(0, r.getHeight(), r.getWidth(), 0, LLColor4(0.f, 0.f, 0.f, 0.35f));

    switch (mode)
    {
        case MODE_WIND_PROFILE: drawWindProfile(); break;
        case MODE_STORM_CELLS:  drawStormCells(); break;
        case MODE_DECK_LOD:     drawDeckLod(); break;
        case MODE_PRECIP_VIRGA: drawVirga(); break;
        case MODE_WEATHER_CUBE: drawWeatherCube(); break;
        case MODE_LIGHTNING:    drawLightning(); break;
        default: break;
    }

    LLView::draw();
}

// The boundary-layer curve: altitude AGL on Y (0 -> cirrus), speed on X, sampled from windAt(z) on the ground-biased ladder; rails at the load-bearing altitudes with live speed and heading; the flowmap-exponent and gradient-setting curves alongside for comparison; and the hodograph inset - wind vector tips per altitude joined into the veer curve.
void SSAtmoGraphView::drawWindProfile()
{
    const SSAtmoInfoView::WindProfileData d = SSAtmoInfoView::windProfileData();
    const LLRect r = getLocalRect();
    const S32 W = r.getWidth();
    const S32 H = r.getHeight();

    if (!d.mValid)
    {
        drawMessage("Wind Profile: no weather cube applied - nothing to profile.");
        return;
    }

    LLFontGL* font = LLFontGL::getFontMonospace();
    if (!font) return;
    const S32 lh = font->getLineHeight();

    ChartBox box;
    box.left   = 58;
    box.bottom = 2 * lh + 8;
    box.w      = W - box.left - 8;
    box.h      = H - box.bottom - (lh + 6);
    if (box.w < 60 || box.h < 60)
    {
        drawMessage("Wind Profile: widen the floater to see the chart.");
        return;
    }

    const F32 top = d.mTopAgl;
    const F32 vmax = d.mMaxSpeed;

    // Title.
    text(llformat("WIND PROFILE  altitude AGL vs speed  ground z %.0f  cirrus %.0f m AGL", d.mGroundZ, top),
         PAD, H - 2, TEXT_NORMAL);

    // Axes.
    gl_line_2d(box.left, box.bottom, box.left, box.bottom + box.h, AXIS);
    gl_line_2d(box.left, box.bottom, box.left + box.w, box.bottom, AXIS);

    // Speed grid: five divisions of the nice ceiling.
    for (S32 i = 0; i <= 5; ++i)
    {
        const F32 v = vmax * (F32)i / 5.f;
        const S32 x = chartX(v, vmax, box);
        if (i > 0) gl_line_2d(x, box.bottom, x, box.bottom + box.h, GRID);
        text(llformat("%.0f", v), x, box.bottom - 2, TEXT_DIM, i == 5);
    }
    text("m/s", box.left + box.w, box.bottom - 2 - lh, TEXT_DIM, true);

    // Altitude grid: 1000 m (500 m on a low ceiling).
    const F32 zstep = (top <= 3000.f) ? 500.f : 1000.f;
    for (F32 z = zstep; z < top; z += zstep)
    {
        const S32 y = chartY(z, top, box);
        gl_line_2d(box.left, y, box.left + box.w, y, GRID);
        text(llformat("%.0f", z), box.left - 4, y + lh / 2, TEXT_DIM, true);
    }
    text("0 m", box.left - 4, box.bottom + lh / 2, TEXT_DIM, true);

    // Curves.
    static LLCachedControl<bool> compare(gSavedSettings, "SSAtmoInfoViewCompare", true);
    auto curve = [&](const SSWindProfile::Params& p, const LLColor4& color, bool dashed)
    {
        std::vector<std::pair<S32, S32> > pts;
        pts.reserve(CURVE_SAMPLES);
        for (S32 i = 0; i < CURVE_SAMPLES; ++i)
        {
            const F32 agl = altitudeLadder(top, CURVE_SAMPLES, i);
            const F32 v = speedOf(SSWindProfile::windAt(agl, p));
            pts.emplace_back(chartX(v, vmax, box), chartY(agl, top, box));
        }
        polyline(pts, color, dashed);
    };
    if (compare)
    {
        SSWindProfile::Params flow = d.mParams;
        flow.mExponent = d.mFlowAlpha;
        curve(flow, CURVE_FLOW, true);
        SSWindProfile::Params grad = d.mParams;
        grad.mExponent = d.mGradientSetting;
        curve(grad, CURVE_GRAD, true);
    }
    curve(d.mParams, CURVE_LIVE, false);

    // Rails with live values. Labels alternate above/below when two rails crowd.
    std::vector<Rail> rails;
    collectRails(d, rails);
    S32 last_label_y = -10000;
    for (const Rail& rail : rails)
    {
        if (!rail.mPresent)
        {
            continue;
        }
        if (rail.mAgl < 0.f || rail.mAgl > top) continue;
        const S32 y = chartY(rail.mAgl, top, box);
        gl_line_2d(box.left, y, box.left + box.w, y, *rail.mColor);
        const SSWindProfile::Vec2 w = SSWindProfile::windAt(rail.mAgl, d.mParams);
        const std::string label = llformat("%s  %.0f m  %.1f m/s  %03.0f", rail.mLabel, rail.mAgl, speedOf(w), headingOfVec(w.x, w.y));
        S32 ly = y + lh + 1; // TOP-aligned text sitting just above the rail
        if (llabs(ly - last_label_y) < lh)
        {
            ly = y - 1;      // crowd: hang it below instead
        }
        text(label, box.left + box.w - 4, ly, *rail.mColor, true);
        last_label_y = ly;
    }
    if (!d.mDeckBuilt)
    {
        text("deck: not built", box.left + 6, box.bottom + box.h - 2, RAIL_BASE);
    }

    // Comparison key, bottom-right inside the chart.
    if (compare)
    {
        S32 ky = box.bottom + 3 * lh + 4;
        text(llformat("live  exp %.2f (cube)", d.mParams.mExponent), box.left + box.w - 4, ky, CURVE_LIVE, true); ky -= lh;
        text(llformat("flowmap region exp %.2f%s", d.mFlowAlpha, d.mFlowSolved ? "" : " (fallback)"), box.left + box.w - 4, ky, CURVE_FLOW, true); ky -= lh;
        text(llformat("gradient setting %.2f", d.mGradientSetting), box.left + box.w - 4, ky, CURVE_GRAD, true);
    }

    // Hodograph inset, top-right of the chart.
    const S32 side = llmin(box.w / 3, box.h / 2);
    if (side >= 70)
    {
        const S32 il = box.left + box.w - side - 6;
        const S32 ib = box.bottom + box.h - side - 6;
        const S32 cx = il + side / 2;
        const S32 cy = ib + side / 2;
        const S32 radius = side / 2 - lh;

        gl_rect_2d(il, ib + side, il + side, ib, LLColor4(0.f, 0.f, 0.f, 0.55f));
        gl_rect_2d(il, ib + side, il + side, ib, GRID, false);
        gl_line_2d(cx - radius, cy, cx + radius, cy, GRID);
        gl_line_2d(cx, cy - radius, cx, cy + radius, GRID);

        std::vector<std::pair<S32, S32> > ring;
        for (S32 i = 0; i <= 32; ++i)
        {
            const F32 a = (F32)i / 32.f * 6.2831853f;
            ring.emplace_back(cx + (S32)floor(std::sin(a) * (F32)radius + 0.5f), cy + (S32)floor(std::cos(a) * (F32)radius + 0.5f));
        }
        polyline(ring, AXIS, false);
        text("N", cx, cy + radius + lh, TEXT_DIM);
        text("E", cx + radius + 2, cy + lh / 2, TEXT_DIM);

        std::vector<std::pair<S32, S32> > hodo;
        hodo.reserve(CURVE_SAMPLES);
        for (S32 i = 0; i < CURVE_SAMPLES; ++i)
        {
            const F32 agl = altitudeLadder(top, CURVE_SAMPLES, i);
            const SSWindProfile::Vec2 w = SSWindProfile::windAt(agl, d.mParams);
            const PixelPoint p = hodoPoint(w.x, w.y, vmax, cx, cy, radius);
            hodo.emplace_back(p.x, p.y);
        }
        polyline(hodo, CURVE_LIVE, false);

        auto dot = [&](F32 agl, const LLColor4& c)
        {
            const SSWindProfile::Vec2 w = SSWindProfile::windAt(agl, d.mParams);
            const PixelPoint p = hodoPoint(w.x, w.y, vmax, cx, cy, radius);
            gl_rect_2d(p.x - 2, p.y + 2, p.x + 2, p.y - 2, c);
        };
        dot(SSWindProfile::REF_M, RAIL_REF);
        dot(SSWindProfile::BL_TOP_M, RAIL_BL);
        if (d.mDeckBuilt)
        {
            dot(d.mBaseZ - d.mGroundZ, RAIL_BASE);
            dot(d.mLidZ - d.mGroundZ, RAIL_LID);
        }
        dot(top, RAIL_CIRRUS);

        text(llformat("hodograph  ring %.0f m/s", vmax), il + 3, ib + side - 2, TEXT_DIM);
    }
}

// V2's chart: the active-cell timeline. One row per cell (hero pinned on top, the rest in id order), a bar across the five lifecycle stages in stage colour with the "now" cursor at age01, the id, supercell flag, age and lifetime; the window's candidate counts in the header. Read-only: it draws the scheduler's frame.
void SSAtmoGraphView::drawStormCells()
{
    const SSAtmoInfoView::StormCellsData d = SSAtmoInfoView::stormCellsData();
    const LLRect r = getLocalRect();
    const S32 W = r.getWidth();
    const S32 H = r.getHeight();

    if (!d.mValid)
    {
        drawMessage("Storm Cells: scheduler not running (no track, region or asset).");
        return;
    }

    LLFontGL* font = LLFontGL::getFontMonospace();
    if (!font) return;
    const S32 lh = font->getLineHeight();

    text(llformat("STORM CELL TIMELINE  %d candidates in window  %d alive  %d spawned  %d super  %d eligible", d.mCandidates, d.mAlive, d.mSpawned,
                  d.mSupercells, d.mTornadoEligible), PAD, H - 2, TEXT_NORMAL);

    const S32 label_w = 150;
    const S32 right_w = 130;
    const S32 bar_left = PAD + label_w;
    const S32 bar_w = W - bar_left - right_w - PAD;
    if (bar_w < 80 || H < 4 * lh)
    {
        drawMessage("Storm Cells: widen the floater to see the timeline.");
        return;
    }

    // Stage scale under the header.
    S32 y = H - 2 - lh - 2;
    static const F32 kBounds[STAGE_COUNT + 1] = { 0.f, 0.15f, 0.30f, 0.55f, 0.80f, 1.f };
    for (S32 s = 0; s < STAGE_COUNT; ++s)
    {
        const S32 x0 = bar_left + (S32)floor(kBounds[s] * (F32)bar_w + 0.5f);
        text(stageLabel(s), x0 + 2, y, toColor(stageColor(s), 1.f));
    }
    y -= lh + 2;

    if (d.mCells.empty())
    {
        text("no active cells", PAD, y, TEXT_DIM);
        if (!d.mHaveHero)
        {
            y -= lh;
            for (const std::string& f : d.mWhyNot)
            {
                text("why not: " + f, PAD, y, TEXT_DIM);
                y -= lh;
                if (y < lh) break;
            }
        }
        return;
    }

    // Row order: hero first, then id order as delivered.
    std::vector<const SSAtmoInfoView::StormCellsData::Cell*> rows;
    rows.reserve(d.mCells.size());
    for (const auto& c : d.mCells) if (c.mIsHero) rows.push_back(&c);
    for (const auto& c : d.mCells) if (!c.mIsHero) rows.push_back(&c);

    const S32 row_h = lh + 4;
    const S32 bar_h = lh - 2;
    for (const auto* c : rows)
    {
        if (y - row_h < 0)
        {
            text(llformat("... %d more", (S32)rows.size()), PAD, y, TEXT_DIM);
            break;
        }
        const S32 bar_top = y - 1;
        const S32 bar_bottom = bar_top - bar_h;

        // Stage bands, bright up to now, dim beyond it.
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        for (S32 s = 0; s < STAGE_COUNT; ++s)
        {
            const S32 x0 = bar_left + (S32)floor(kBounds[s] * (F32)bar_w + 0.5f);
            const S32 x1 = bar_left + (S32)floor(kBounds[s + 1] * (F32)bar_w + 0.5f);
            const S32 xn = bar_left + (S32)floor(llclamp(c->mAge01, kBounds[s], kBounds[s + 1]) * (F32)bar_w + 0.5f);
            const LLColor4 bright = toColor(stageColor(s), 0.9f);
            const LLColor4 dim = toColor(stageColor(s), 0.25f);
            if (xn > x0) gl_rect_2d(x0, bar_top, xn, bar_bottom, bright);
            if (x1 > xn) gl_rect_2d(xn, bar_top, x1, bar_bottom, dim);
        }
        // Now cursor.
        const S32 xnow = bar_left + (S32)floor(llclamp(c->mAge01, 0.f, 1.f) * (F32)bar_w + 0.5f);
        gl_line_2d(xnow, bar_top + 2, xnow, bar_bottom - 2, TEXT_NORMAL);

        const LLColor4 label_c = c->mIsForced ? FORCED_OUTLINE : (c->mIsHero ? HERO_ORIGIN : (c->mSupercell ? toColor(rotationRamp(c->mRotation), 1.f) : TEXT_NORMAL));
        text(llformat("%s%s", c->mIsHero ? "H " : (c->mSupercell ? "S " : "  "), shortId(c->mId).c_str()), PAD, y, label_c);
        const F64 age = d.mNow - c->mBirthTime;
        // <SS:Nexii> SQUALL/FORCED: the same tag renderStormCells' own labels carry, so the timeline and the
        // in-world layer read the same cell the same way.
        const std::string tag = c->mIsForced ? "  FORCED" : (c->mLineId != 0 ? llformat("  line %s", shortId(c->mLineId).c_str()) : std::string());
        text(llformat("%.0f/%.0f min  I %.2f%s", age / 60.0, c->mLifetimeS / 60.f, c->mIntensity, tag.c_str()), W - PAD, y, TEXT_DIM, true);
        y -= row_h;
    }
}

// V3's chart: SSDeckLod::keepFrac plotted against camera distance out to DECK_EDGE_M, with the five LOD rails as
// vertical lines in ring colour and the header carrying the same placed/budget/predicted figures the legend
// shows - a pure function of the core, so this curve is the SAME one the tile tint and the in-world rings draw
// from, just read off the axis directly rather than eyeballed off a grey swatch.
void SSAtmoGraphView::drawDeckLod()
{
    const SSAtmoInfoView::DeckLodData d = SSAtmoInfoView::deckLodData();
    const LLRect r = getLocalRect();
    const S32 W = r.getWidth();
    const S32 H = r.getHeight();

    if (!d.mDeckBuilt)
    {
        drawMessage("Deck LOD: no cloud deck built.");
        return;
    }

    LLFontGL* font = LLFontGL::getFontMonospace();
    if (!font) return;
    const S32 lh = font->getLineHeight();

    ChartBox box;
    box.left   = 58;
    box.bottom = 2 * lh + 8;
    box.w      = W - box.left - 8;
    box.h      = H - box.bottom - (lh + 6);
    if (box.w < 60 || box.h < 60)
    {
        drawMessage("Deck LOD: widen the floater to see the chart.");
        return;
    }

    const F32 dmax = SSDeckLod::DECK_EDGE_M;

    text(llformat("DECK LOD  keep fraction vs camera distance  placed %d / budget %d  dial %d  predicted %lld/%d cells",
                  d.mPuffsPlaced, d.mBudget, d.mPuffsPerCell, (long long)d.mLodPredicted, d.mCellsWalked),
         PAD, H - 2, TEXT_NORMAL);

    gl_line_2d(box.left, box.bottom, box.left, box.bottom + box.h, AXIS);
    gl_line_2d(box.left, box.bottom, box.left + box.w, box.bottom, AXIS);

    for (S32 i = 0; i <= 5; ++i)
    {
        const F32 dd = dmax * (F32)i / 5.f;
        const S32 x = box.left + (S32)floor((dd / dmax) * (F32)box.w + 0.5f);
        if (i > 0) gl_line_2d(x, box.bottom, x, box.bottom + box.h, GRID);
        text(llformat("%.0f", dd), x, box.bottom - 2, TEXT_DIM, i == 5);
    }
    text("m", box.left + box.w, box.bottom - 2 - lh, TEXT_DIM, true);

    for (S32 i = 0; i <= 4; ++i)
    {
        const F32 kf = (F32)i / 4.f;
        const S32 y = box.bottom + (S32)floor(kf * (F32)box.h + 0.5f);
        gl_line_2d(box.left, y, box.left + box.w, y, GRID);
        text(llformat("%.2f", kf), box.left - 4, y + lh / 2, TEXT_DIM, true);
    }

    std::vector<std::pair<S32, S32> > pts;
    pts.reserve(CURVE_SAMPLES);
    for (S32 i = 0; i < CURVE_SAMPLES; ++i)
    {
        const F32 dd = dmax * (F32)i / (F32)(CURVE_SAMPLES - 1);
        const F32 kf = SSDeckLod::keepFrac(dd);
        const S32 x = box.left + (S32)floor((dd / dmax) * (F32)box.w + 0.5f);
        const S32 y = box.bottom + (S32)floor(kf * (F32)box.h + 0.5f);
        pts.emplace_back(x, y);
    }
    polyline(pts, CURVE_LIVE, false);

    auto rail = [&](F32 dd, const char* label, const LLColor4& c)
    {
        const S32 x = box.left + (S32)floor((dd / dmax) * (F32)box.w + 0.5f);
        gl_line_2d(x, box.bottom, x, box.bottom + box.h, c);
        text(label, x + 2, box.bottom + box.h - 2, c);
    };
    rail(SSDeckLod::SUBS_FULL_M, "full", RING_SUBS_FULL);
    rail(SSDeckLod::SUBS_TWO_M, "2->1", RING_SUBS_TWO);
    rail(SSDeckLod::THIN_START_M, "thin", RING_THIN_START);
    rail(SSDeckLod::FIELD_FADE_START_M, "fade", RING_FADE_START);
}

// V5's chart: the day-cycle cube in two lanes (SSAtmoInfoViewCore::cubeLaneBox) - the top lane the AUTHORED curves
// (moisture, convection, temperature, wind speed/heading, shear strength/veer, every one of them read through the
// SAME resolvers the sky itself samples - SSAtmoEnvWeatherResolver::resolve, weatherCubeData()'s own comment), the
// bottom lane the DERIVED gates (storm consolidation, puff gloom, anvil ramp, the lightning gate, and the storm
// spawn score at the anchor's own lattice cell) - with the "now" cursor spanning both lanes and a marker at every
// authored override cue (mStormOverride / mPrecipitationOverride). Magnitude curves in the top lane are each
// normalised to their own domain (moisture/convection/shear are already unit; temperature and wind speed get a
// nice-ceiling domain over this cube's own samples, SSAtmoInfoViewCore::niceMax/unitOf, the SAME helpers V1's
// chart uses); a magnitude's paired ANGLE (heading for wind, veer for shear) draws DASHED in the same colour -
// solid means magnitude, dashed means the angle that rides with it. Every bottom-lane curve is already unit
// (0..1), so it shares the lane's axis directly with no per-curve domain.
void SSAtmoGraphView::drawWeatherCube()
{
    const SSAtmoInfoView::WeatherCubeData d = SSAtmoInfoView::weatherCubeData();
    const LLRect r = getLocalRect();
    const S32 W = r.getWidth();
    const S32 H = r.getHeight();

    if (!d.mValid)
    {
        drawMessage("Weather Cube: no weather cube applied - nothing to chart.");
        return;
    }

    LLFontGL* font = LLFontGL::getFontMonospace();
    if (!font) return;
    const S32 lh = font->getLineHeight();

    text(llformat("WEATHER CUBE  track '%s'  day length %.0f min  now phase %.3f  %d cue%s", d.mTrackName.c_str(),
                  d.mDayLengthS / 60.0, d.mNowPhase, (S32)d.mCues.size(), d.mCues.size() == 1 ? "" : "s"),
         PAD, H - 2, TEXT_NORMAL);

    ChartBox outer;
    outer.left   = 58;
    outer.bottom = 2 * lh + 8;
    outer.w      = W - outer.left - 8;
    outer.h      = H - outer.bottom - (lh + 6);
    if (outer.w < 80 || outer.h < 100 || d.mSamples.empty())
    {
        drawMessage("Weather Cube: widen the floater to see the chart.");
        return;
    }

    const ChartBox top_box = cubeLaneBox(outer, 0, 2, 16);
    const ChartBox bot_box = cubeLaneBox(outer, 1, 2, 16);

    // Per-cube domains for the top lane's two non-unit magnitudes, over THIS cube's own samples - the same
    // "nice ceiling" niceMax gives V1's wind-speed axis, and a rounded-out temperature span so a near-flat day
    // still gets a readable axis rather than one that hugs a single degree.
    F32 temp_min = d.mSamples[0].mTemperatureC, temp_max = d.mSamples[0].mTemperatureC, wind_peak = 0.f;
    for (const auto& s : d.mSamples)
    {
        temp_min = llmin(temp_min, s.mTemperatureC);
        temp_max = llmax(temp_max, s.mTemperatureC);
        wind_peak = llmax(wind_peak, s.mWindSpeed);
    }
    temp_min = llmin(temp_min, 0.f) - 5.f;
    temp_max = llmax(temp_max, temp_min + 10.f) + 5.f;
    const F32 wind_ceiling = niceMax(llmax(wind_peak, 1.f));

    // A generic lane frame: axes, four quarter-phase gridlines, and the 0/1 (or domain-end) value labels.
    auto laneFrame = [&](const ChartBox& box, const char* title, const char* lo_label, const char* hi_label)
    {
        text(title, box.left, box.bottom + box.h - 2, TEXT_NORMAL);
        gl_line_2d(box.left, box.bottom, box.left, box.bottom + box.h, AXIS);
        gl_line_2d(box.left, box.bottom, box.left + box.w, box.bottom, AXIS);
        for (S32 q = 1; q < 4; ++q)
        {
            const S32 x = cubePhaseX((F64)q / 4.0, box);
            gl_line_2d(x, box.bottom, x, box.bottom + box.h, GRID);
        }
        text(lo_label, box.left - 4, box.bottom + lh / 2, TEXT_DIM, true);
        text(hi_label, box.left - 4, box.bottom + box.h + lh / 2, TEXT_DIM, true);
    };
    laneFrame(top_box, "day-cycle curves (solid = magnitude, dashed = paired angle)", "0", "1 / max");
    laneFrame(bot_box, "derived gates", "0", "1");

    // Plots a per-sample unit-space series (already mapped to [0,1] by the caller) as a polyline in `box`.
    auto plot = [&](const ChartBox& box, const std::vector<F32>& unit_values, const LLColor4& color, bool dashed)
    {
        std::vector<std::pair<S32, S32> > pts;
        pts.reserve(d.mSamples.size());
        for (size_t i = 0; i < d.mSamples.size(); ++i)
        {
            pts.emplace_back(cubePhaseX((F64)d.mSamples[i].mPhase, box), chartY(unit_values[i], 1.f, box));
        }
        polyline(pts, color, dashed);
    };

    const size_t n = d.mSamples.size();
    std::vector<F32> moisture(n), convection(n), temp(n), wind_speed(n), wind_heading(n), shear(n), veer(n);
    std::vector<F32> consolidation(n), gloom(n), anvil(n), lightning(n), storm_score(n);
    for (size_t i = 0; i < n; ++i)
    {
        const auto& s = d.mSamples[i];
        moisture[i]      = s.mMoisture;
        convection[i]    = s.mConvection;
        temp[i]           = unitOf(s.mTemperatureC - temp_min, temp_max - temp_min);
        wind_speed[i]     = unitOf(s.mWindSpeed, wind_ceiling);
        wind_heading[i]   = unitOf(s.mWindHeading, 360.f);
        shear[i]          = llclamp(s.mShearStrength, 0.f, 1.f);
        veer[i]           = unitOf(s.mVeerDeg + 180.f, 360.f);
        consolidation[i]  = s.mConsolidation;
        gloom[i]          = s.mGloom;
        anvil[i]          = s.mAnvilRamp;
        lightning[i]      = s.mLightningIntensity;
        storm_score[i]    = s.mStormScore;
    }

    plot(top_box, moisture, CUBE_MOISTURE, false);
    plot(top_box, convection, CUBE_CONVECTION, false);
    plot(top_box, temp, CUBE_TEMP, false);
    plot(top_box, wind_speed, CUBE_WIND, false);
    plot(top_box, wind_heading, CUBE_WIND, true);
    plot(top_box, shear, CUBE_SHEAR, false);
    plot(top_box, veer, CUBE_SHEAR, true);

    plot(bot_box, consolidation, CUBE_CONSOLIDATE, false);
    plot(bot_box, gloom, CUBE_GLOOM, false);
    plot(bot_box, anvil, CUBE_ANVIL, false);
    plot(bot_box, lightning, CUBE_LIGHTNING, false);
    if (d.mHaveStormScore)
    {
        plot(bot_box, storm_score, CUBE_STORM_SCORE, false);
    }

    // Phase-of-cycle labels under the bottom lane only (the two lanes share one phase axis).
    for (S32 q = 0; q <= 4; ++q)
    {
        const F32 phase = (F32)q / 4.f;
        const S32 x = cubePhaseX((F64)phase, bot_box);
        text(llformat("%.2f", phase), x, bot_box.bottom - 2, TEXT_DIM, q == 4);
    }
    text("phase of cycle", outer.left + outer.w / 2, bot_box.bottom - 2 - lh, TEXT_DIM);

    // The "now" cursor, spanning both lanes.
    {
        const S32 x = cubePhaseX(d.mNowPhase, outer);
        gl_line_2d(x, outer.bottom, x, outer.bottom + outer.h, CUBE_NOW);
        text(llformat("now %.3f", d.mNowPhase), x + 3, outer.bottom + outer.h - 2, CUBE_NOW);
    }

    // Authored override cue markers, spanning both lanes, alternating label height so adjacent cues stay legible.
    S32 cue_row = 0;
    for (const auto& cue : d.mCues)
    {
        const LLColor4& c = cue.mIsStorm ? CUBE_CUE_STORM : CUBE_CUE_PRECIP;
        const S32 x = cubePhaseX(cue.mPhase, outer);
        gl_line_2d(x, outer.bottom, x, outer.bottom + outer.h, c);
        const S32 ly = outer.bottom + outer.h - lh - (cue_row % 3) * lh;
        text(cue.mLabel, x + 3, ly, c);
        ++cue_row;
    }
}

// V4's chart (doc/atmo_magic_debug_views.md V4; the panel backlog item 5.3 says the shipped in-world layer never
// got). Four questions, all answered from the LAST BUILD's own snapshot - SSVolCloud::virgaDebug() read straight
// across by SSAtmoInfoView::virgaData(), never re-derived here:
//   1. the shaft POPULATION and its budget headroom - a bar of kept against SSVirga::MAX_SHAFTS, plus the number
//      of candidates the hashed trim had to choose from and the probability it applied to each of them
//      (SSAtmoInfoViewCore::keepProbability of SSVirga::quantiseCount, the trim's own two-step formula, held to
//      the shipping SSVirga::keepHash by tests/xsection_infoview_virga_budget.cpp);
//   2. the DRIVE distribution - a histogram of every qualifying cell's drive, the kept share drawn bright inside
//      the dim total, so "the trim ate my heavy cells" is a shape rather than a suspicion;
//   3. the TIER HANDOFF - SSVirga::handoff plotted against camera distance, with rails at the particle rain's own
//      sheets radius r2, at the r2 x HANDOFF_SKIP line where shaft cards start at all, and at the end of the
//      HANDOFF_BAND_M ramp where they reach full alpha: the distance at which the two rain systems change hands;
//   4. whether the numbers are even live (no cube / Distant Rain off / no deck built each say so instead).
void SSAtmoGraphView::drawVirga()
{
    const SSAtmoInfoView::VirgaData d = SSAtmoInfoView::virgaData();
    const LLRect r = getLocalRect();
    const S32 W = r.getWidth();
    const S32 H = r.getHeight();

    if (!d.mValid)
    {
        drawMessage("Precip & Virga: no weather cube applied.");
        return;
    }
    if (!d.mActive)
    {
        drawMessage("Precip & Virga: Distant Rain off, or this deck is not the storm-coupled one.");
        return;
    }

    LLFontGL* font = LLFontGL::getFontMonospace();
    if (!font) return;
    const S32 lh = font->getLineHeight();

    // The trim's own two numbers: the quantised candidate count it hashed against, and the probability that gave
    // each cell. SSVirga owns the quantiser; the info core only turns its answer into the percentage printed here.
    const F32 quantised = SSVirga::quantiseCount(d.mCandidates);
    const F32 keep_p = keepProbability(SSVirga::MAX_SHAFTS, quantised);

    text(llformat("PRECIP & VIRGA  %d qualifying  kept %d  trimmed %d  trim p %.0f%% (n->%.0f)  r2 %.0f m%s",
                  d.mCandidates, d.mKept, d.mTrimmed, keep_p * 100.f, quantised, d.mR2,
                  d.mDeckBuilt ? "" : "  (deck not built)"),
         PAD, H - 2, TEXT_NORMAL);

    // ---- the budget bar: kept against MAX_SHAFTS, with the overshoot the rank-cut backstop allows printed as a
    // number rather than drawn past the bar's own end (SSAtmoInfoViewCore::budgetFrac / budgetFillPx).
    const S32 bar_h = lh - 2;
    const S32 bar_w = llmin(220, W - PAD * 2 - 200);
    const S32 bar_y = H - 2 - lh - 4;
    if (bar_w > 40)
    {
        const S32 fill = budgetFillPx(d.mKept, SSVirga::MAX_SHAFTS, bar_w);
        const F32 frac = budgetFrac(d.mKept, SSVirga::MAX_SHAFTS);
        gl_rect_2d(PAD, bar_y, PAD + bar_w, bar_y - bar_h, LLColor4(1.f, 1.f, 1.f, 0.10f));
        gl_rect_2d(PAD, bar_y, PAD + fill, bar_y - bar_h,
                   (frac > 1.f) ? LLColor4(1.f, 0.35f, 0.30f, 0.85f)
                                : LLColor4(RAIL_BASE.mV[0], RAIL_BASE.mV[1], RAIL_BASE.mV[2], 0.85f));
        gl_line_2d(PAD + bar_w, bar_y, PAD + bar_w, bar_y - bar_h, AXIS);
        const S32 headroom = SSVirga::MAX_SHAFTS - d.mKept;
        text((headroom >= 0) ? llformat("budget %d / %d   headroom %d   (hard cap %d)", d.mKept, SSVirga::MAX_SHAFTS,
                                        headroom, SSVirga::hardCap(SSVirga::MAX_SHAFTS))
                             : llformat("budget %d / %d   OVER by %d   (hard cap %d)", d.mKept, SSVirga::MAX_SHAFTS,
                                        -headroom, SSVirga::hardCap(SSVirga::MAX_SHAFTS)),
             PAD + bar_w + 8, bar_y + 1, (frac > 1.f) ? LLColor4(1.f, 0.55f, 0.45f, 1.f) : TEXT_DIM);
    }

    ChartBox outer;
    outer.left   = 46;
    outer.bottom = 2 * lh + 8;
    outer.w      = W - outer.left - 8;
    outer.h      = H - outer.bottom - (2 * lh + bar_h + 10);
    if (outer.w < 60 || outer.h < 70)
    {
        drawMessage("Precip & Virga: widen the floater to see the chart.");
        return;
    }

    const ChartBox top_box = cubeLaneBox(outer, 0, 2, 18);
    const ChartBox bot_box = cubeLaneBox(outer, 1, 2, 18);

    // ---- top lane: the drive histogram. Bars are the TOTAL qualifying count per drive bucket in a flat grey; the
    // kept share is drawn inside each bar in the presence ramp at that bucket's own drive, so the trim's bite is
    // visible bucket by bucket.
    S32 total[DRIVE_BUCKETS] = { 0 };
    S32 kept[DRIVE_BUCKETS] = { 0 };
    for (const auto& c : d.mCells)
    {
        const S32 b = driveBucket(c.mDrive, DRIVE_BUCKETS);
        total[b] += 1;
        if (c.mKept) kept[b] += 1;
    }
    S32 tallest = 0;
    for (S32 i = 0; i < DRIVE_BUCKETS; ++i) tallest = llmax(tallest, total[i]);

    gl_line_2d(top_box.left, top_box.bottom, top_box.left, top_box.bottom + top_box.h, AXIS);
    gl_line_2d(top_box.left, top_box.bottom, top_box.left + top_box.w, top_box.bottom, AXIS);
    for (S32 i = 0; i < DRIVE_BUCKETS; ++i)
    {
        const ChartBox tb = histogramBarBox(top_box, i, DRIVE_BUCKETS, total[i], tallest, 2);
        const ChartBox kb = histogramBarBox(top_box, i, DRIVE_BUCKETS, kept[i], tallest, 2);
        if (tb.h > 0)
        {
            gl_rect_2d(tb.left, tb.bottom + tb.h, tb.left + tb.w, tb.bottom, LLColor4(0.55f, 0.55f, 0.55f, 0.45f));
        }
        if (kb.h > 0)
        {
            const F32 mid_drive = ((F32)i + 0.5f) / (F32)DRIVE_BUCKETS;
            gl_rect_2d(kb.left, kb.bottom + kb.h, kb.left + kb.w, kb.bottom, toColor(presenceRamp(mid_drive, 1.f), 0.9f));
        }
    }
    // The qualify threshold, as a rail on the drive axis. It is the UNSCALED constant: the live gate divides it by
    // the Weather Influence "Distant rain" strength, which this snapshot does not carry, so the rail is labelled
    // for what it is rather than moved to a number the view cannot know.
    {
        const S32 x = top_box.left + (S32)floor(llclamp(SSVirga::THRESHOLD, 0.f, 1.f) * (F32)top_box.w + 0.5f);
        gl_line_2d(x, top_box.bottom, x, top_box.bottom + top_box.h, RING_FADE_START);
        text(llformat("thr %.2f (unscaled)", SSVirga::THRESHOLD), x + 2, top_box.bottom + top_box.h - 2, RING_FADE_START);
    }
    text(llformat("drive histogram  tallest bucket %d  (inner bar = kept by the hash trim)", tallest),
         top_box.left + 2, top_box.bottom + top_box.h + lh, TEXT_DIM);
    for (S32 q = 0; q <= 5; ++q)
    {
        const F32 dv = (F32)q / 5.f;
        const S32 x = top_box.left + (S32)floor(dv * (F32)top_box.w + 0.5f);
        text(llformat("%.1f", dv), x, top_box.bottom - 2, TEXT_DIM, q == 5);
    }

    // ---- bottom lane: the tier handoff. The curve is SSVirga::handoff itself - the same function the emitter's
    // own per-card alpha multiplies by - sampled across a distance axis sized to leave the ramp's end inboard.
    const F32 skip_r = d.mR2 * SSVirga::HANDOFF_SKIP;
    const F32 ramp_end = skip_r + SSVirga::HANDOFF_BAND_M;
    const F32 axis_max = distanceAxisMaxM(ramp_end);

    gl_line_2d(bot_box.left, bot_box.bottom, bot_box.left, bot_box.bottom + bot_box.h, AXIS);
    gl_line_2d(bot_box.left, bot_box.bottom, bot_box.left + bot_box.w, bot_box.bottom, AXIS);

    std::vector<std::pair<S32, S32> > pts;
    pts.reserve(CURVE_SAMPLES);
    for (S32 i = 0; i < CURVE_SAMPLES; ++i)
    {
        const F32 dist = axis_max * (F32)i / (F32)(CURVE_SAMPLES - 1);
        const F32 h = SSVirga::handoff(dist, d.mR2);
        pts.emplace_back(chartX(dist, axis_max, bot_box), chartY(h, 1.f, bot_box));
    }
    polyline(pts, CURVE_LIVE, false);

    auto rail = [&](F32 dist, const char* label, const LLColor4& c, S32 label_row)
    {
        if (dist <= 0.f || dist > axis_max) return;
        const S32 x = chartX(dist, axis_max, bot_box);
        gl_line_2d(x, bot_box.bottom, x, bot_box.bottom + bot_box.h, c);
        text(llformat("%s %.0f m", label, dist), x + 2, bot_box.bottom + bot_box.h - 2 - label_row * lh, c);
    };
    rail(d.mR2, "r2 sheets", VIRGA_HANDOFF, 0);
    rail(skip_r, "shafts start", RAIL_BASE, 1);
    rail(ramp_end, "full shaft alpha", RING_THIN_START, 2);

    for (S32 q = 0; q <= 5; ++q)
    {
        const F32 dist = axis_max * (F32)q / 5.f;
        const S32 x = chartX(dist, axis_max, bot_box);
        text(llformat("%.0f", dist), x, bot_box.bottom - 2, TEXT_DIM, q == 5);
    }
    text("camera distance (m)  -  particle rain inside the ramp, shaft cards outside it",
         bot_box.left + 2, bot_box.bottom - 2 - lh, TEXT_DIM);
}

// V9's chart: the strike timeline. One row per live strike - a bar across the five lifecycle stages
// (SSAtmoInfoViewCore::strikeStage's own table) with this strike's stage lit and the ones behind it filled, the
// kind and polarity, its clock (a countdown before contact, an age after it) and whether the cloud deck is lit by
// it - plus a header carrying the schedule and the two light budgets, and the gate's own values when nothing is
// alive. Everything is read from the model's resident fields through SSAtmoInfoView::lightningData(); the chart
// never advances a strike or asks for one.
void SSAtmoGraphView::drawLightning()
{
    const SSAtmoInfoView::LightningData d = SSAtmoInfoView::lightningData();
    const LLRect r = getLocalRect();
    const S32 W = r.getWidth();
    const S32 H = r.getHeight();

    if (!d.mValid)
    {
        drawMessage("Lightning: system not running.");
        return;
    }

    LLFontGL* font = LLFontGL::getFontMonospace();
    if (!font) return;
    const S32 lh = font->getLineHeight();

    text(llformat("STRIKE TIMELINE  %d live  next %s  deck lit %d/%d  scene lights %d/%d",
                  (S32)d.mStrikes.size(),
                  (d.mNextIn >= 0.0) ? llformat("in %.1fs", d.mNextIn).c_str() : "not scheduled",
                  d.mCloudLit, d.mCloudCap, (S32)d.mLights.size(), d.mSceneLightCap),
         PAD, H - 2, TEXT_NORMAL);

    const S32 label_w = 132;
    const S32 right_w = 150;
    const S32 bar_left = PAD + label_w;
    const S32 bar_w = W - bar_left - right_w - PAD;
    if (bar_w < 80 || H < 4 * lh)
    {
        drawMessage("Lightning: widen the floater to see the timeline.");
        return;
    }

    // The stage scale under the header: five equal steps, the same table and the same colours the channels are
    // drawn in, so a row's bar is read against the ladder every strike shares.
    S32 y = H - 2 - lh - 2;
    for (S32 st = 0; st < STRIKE_STAGE_COUNT; ++st)
    {
        const S32 x0 = bar_left + (bar_w * st) / STRIKE_STAGE_COUNT;
        text(strikeStageLabel(st), x0 + 2, y, toColor(strikeStageColor(st), 1.f));
    }
    y -= lh + 2;

    if (d.mStrikes.empty())
    {
        text("no live strikes", PAD, y, TEXT_DIM);
        y -= lh;
        text(llformat("why not: lightning %s   intensity %.2f   interval %.0f-%.0f s",
                      d.mEnabled ? "ON" : "OFF", d.mIntensity, d.mIntervalMinS, d.mIntervalMaxS), PAD, y, TEXT_DIM);
        y -= lh;
        text((d.mNextIn >= 0.0) ? llformat("next strike scheduled in %.1f s", d.mNextIn)
                                : std::string("no strike scheduled"), PAD, y, TEXT_DIM);
        return;
    }

    const S32 row_h = lh + 4;
    const S32 seg_h = lh - 2;
    for (const auto& s : d.mStrikes)
    {
        if (y < row_h) break;

        text(llformat("%s %s", strikeKindLabel(s.mKind), s.mPositive ? "+" : "-"), PAD, y,
             toColor(strikeStageColor(s.mStage), 1.f));

        // The stage bar: every stage up to and including this one filled, the current one at full alpha.
        for (S32 st = 0; st < STRIKE_STAGE_COUNT; ++st)
        {
            const S32 x0 = bar_left + (bar_w * st) / STRIKE_STAGE_COUNT;
            const S32 x1 = bar_left + (bar_w * (st + 1)) / STRIKE_STAGE_COUNT;
            const F32 alpha = (st < s.mStage) ? 0.35f : ((st == s.mStage) ? 0.95f : 0.10f);
            gl_rect_2d(x0, y - 2, x1 - 2, y - 2 - seg_h, toColor(strikeStageColor(st), alpha));
        }

        // Within-stage detail: the leader's own progress is the one sub-stage with a real fraction behind it, so
        // it gets a cursor inside its own segment rather than a second bar.
        if (s.mStage == STRIKE_STAGE_LEADER)
        {
            const S32 x0 = bar_left + (bar_w * STRIKE_STAGE_LEADER) / STRIKE_STAGE_COUNT;
            const S32 x1 = bar_left + (bar_w * (STRIKE_STAGE_LEADER + 1)) / STRIKE_STAGE_COUNT;
            const S32 cx = x0 + (S32)floor(llclamp(s.mLeaderProgress, 0.f, 1.f) * (F32)(x1 - x0) + 0.5f);
            gl_line_2d(cx, y - 2, cx, y - 2 - seg_h, TEXT_NORMAL);
        }

        const std::string when = (s.mT < 0.f) ? llformat("in %.2fs", -s.mT) : llformat("+%.2fs", s.mT);
        text(llformat("%s  I %.2f  %.0f m%s", when.c_str(), s.mIntensity, s.mDistanceM,
                      s.mLightsCloud ? "  deck" : ""),
             W - PAD, y, TEXT_DIM, true);
        y -= row_h;
    }
}
