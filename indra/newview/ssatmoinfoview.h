/**
 * @file ssatmoinfoview.h
 * @brief Atmo Magic: the info-view framework (dim, legend, graph widget, mode plumbing) and V1 Wind Profile.
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

#ifndef SS_ATMOINFOVIEW_H
#define SS_ATMOINFOVIEW_H

#include "llview.h"
#include "sssoundscape.h"
#include "ssstormcellcore.h"
#include "sswindprofilecore.h"
#include "v2math.h"
#include "v3color.h" // <SS:Nexii> V9: LightningData::SceneLight carries sceneLights' own LLColor3
#include "v3math.h"  // <SS:Nexii> V9: strike origins, attachment points and light positions

#include <boost/signals2.hpp>
#include <string>
#include <utility>
#include <vector>

// <SS:Nexii> Cities: Skylines-style info views for Atmo Magic (doc/atmo_magic_debug_views.md). One exclusive mode at a time (the SSAtmoInfoView setting), the world dims under a translucent quad at the top of the UI stage, a shared legend explains the colours, a graph widget on the debug floater's Views tab draws what a 3D overlay cannot, and the mode's in-world layer is drawn post-deferred beside the engineering overlays. READ-ONLY by contract: every view reads counters and fields the systems already hold - it never ticks, builds or reorders anything, and says "not built" when the data is not resident. Nothing drawn here positions world content, so the camera is free to shape it.
class SSAtmoInfoView
{
public:
    // Creates the (now draw-less) dim view, the legend, and the chart (drawn last) as children of the debug view - the dim quad itself draws in the 3-D pass, see renderDimAndWorld -, and starts driving the engineering masks off the mode setting. The chart is docked to the legend's right edge (its own draw() re-reads the legend's rect every frame and repositions off it, the same way the legend re-sizes itself off its own content), NOT parked at a fixed offset - the legend's width changes with its spec's row count, so a fixed gap would either overlap it or leave a gap that grows. Called once from LLDebugView::init. [interaction: SSAtmoLegendView] [interaction: SSAtmoGraphView]
    static void attach(LLView* debug_view);

    // The live mode, 0 when off (SSAtmoInfoViewCore::MODE_*).
    static U32 mode();

    // <SS:Nexii> Whether the 3-D half has anything to draw at all, asked once per frame by render_ui() in
    // llviewerdisplay.cpp ahead of renderDimAndWorld. It is NOT just "a mode is selected" any more: V9's layer
    // also answers to RENDER_DEBUG_LIGHTNING, the engineering checkbox bound beside the other seven overlays, so
    // the mask keeps the overlay alive with no mode picked. Kept here rather than spelled out at the call site so
    // a future view that gains its own mask never needs llviewerdisplay.cpp edited again. [interaction: render_ui]
    static bool wantsDraw();

    // The active mode's in-world layer (V1: the wind mast; V2: lattice tiles, cell rings, squall-line bars/junctions, forced outlines, hero ribbon, rotation glyphs; V4: shaft cells, tilt/skew lines, the handoff ring; V5 has none - a pure chart; V9: strike channels, crawl, attachment, occlusion box and the light readings). Draws nothing when off - EXCEPT V9's layer, which also answers to RENDER_DEBUG_LIGHTNING and so draws with no mode selected; it is dispatched exactly once either way. Called only from renderDimAndWorld(), i.e. post-tonemap, from a 3-D pass with the world camera, AFTER that function has laid the look pass down. [interaction: render_ui]
    static void renderWorld();

    // <SS:Nexii> The overlay's whole 3-D half in one call, from render_ui() in llviewerdisplay.cpp - post-tonemap, a 3-D pass with the world camera, between gPipeline.renderFinalize() and render_hud_attachments(): the LOOK pass (renderInfoLook, a full-screen triangle that rewrites the presented world as warm gray) and then renderWorld() on top of it. NOT from LLPipeline::renderDebug any more - that runs inside renderGeomPostDeferred, so the overlay landed in the HDR screen buffer ahead of the luminance sample and auto-exposure fought it. The 2-D half - legend and chart - stays in the UI stage as ordinary LLViews. Draws nothing when off; the caller also guards on gCubeSnapshot/gSnapshot/gDisconnected and on the UI debug-feature mask. [interaction: render_ui] [interaction: SSAtmoDimView]
    static void renderDimAndWorld();

    // <SS:Nexii> V1's data, gathered once per draw from the systems' resident state: the applier's wind profile and floors, the deck's built band, the flowmap's region exponent. mValid is false with no applied weather cube; mDeckBuilt is false when the volumetric field has no puffs (the rails then read "not built" instead of forcing one). Read-only by construction - every accessor it touches is a const getter. [interaction: SSAtmoEnvApplier windProfile/windAt] [interaction: SSVolCloud cloudBaseZ/cloudTopZ] [interaction: SSWindFlowMap windAlpha]
    struct WindProfileData
    {
        bool mValid = false;
        bool mDeckBuilt = false;
        bool mFlowSolved = false;
        SSWindProfile::Params mParams;
        F32 mGroundZ = 0.f;      // the track floor windAt measures AGL from
        F32 mBaseZ = 0.f;        // the deck base the drift is integrated at
        F32 mLidZ = 0.f;         // the primary deck's top (valid with mDeckBuilt)
        F32 mCirrusZ = 0.f;      // the dome/cirrus band's CURRENT altitude (moves with the anvil ramp)
        F32 mTopAgl = 1.f;       // chart ceiling AGL: the cirrus altitude, floored above the boundary layer
        F32 mMaxSpeed = 1.f;     // nice axis ceiling over the profile's fastest sample
        F32 mFlowAlpha = 0.f;    // the flowmap's camera-region exponent (or its fallback)
        F32 mGradientSetting = 0.f; // the SSAtmoWindFlowGradient fallback
    };
    static WindProfileData windProfileData();

    // <SS:Nexii> V2's data (doc/atmo_magic_debug_views.md V2), gathered once per draw from the storm scheduler's RESIDENT frame: SSStormCells::cells()/hero()/whyNot() exactly as update() left them, plus the lattice tiles within FIELD_M of the anchor re-read through the pure core (SSStormCell::candidate at the current epoch - a pure function of the published seed, lattice index and epoch, so reading it here cannot spawn, steer or reorder anything), plus - DEBUG - SSVortices::active()/dustDevils()/whyNot() exactly as ITS update() left them (mVortices/mDustDevils/mVortexWhyNot; empty/default when SSVortices is not valid this frame). Positions are converted global -> agent frame at this read site (SSStormCells::toAgentXY), the frame SSVolCloud positions puffs in; the storm/vortex entities themselves stay world-frame. mValid is false when the scheduler is not driving; the layer then says so instead of asking it to. Read-only by construction. [interaction: SSStormCells] [interaction: SSAtmoEnvManager mWeatherInfluence Allow flags] [interaction: SSVolCloud cloudBaseZ] [interaction: SSVortices active/dustDevils/whyNot]
    struct StormCellsData
    {
        struct Tile
        {
            LLVector2 mCentreAgent;
            F32 mPotential = 0.f;    // P this epoch
            F32 mShearNoise = 0.f;   // SH this epoch
            bool mAlive = false;     // an active cell was born on this lattice cell (any epoch in the window)
            // <SS:Nexii> V2 lattice-tint smoothing: the lattice index this tile enumerated at (SSStormCell::
            // enumerateLattice's own lx/ly), kept so renderStormCells can look a neighbouring tile's mPotential up
            // by index for the per-corner average - display smoothing only, never fed back into mPotential itself.
            S32 mLX = 0, mLY = 0;
        };
        struct Cell
        {
            U64 mId = 0;
            LLVector2 mCentreAgent;
            F32 mRadiusM = 0.f;
            S32 mStage = 0;          // SSStormCell::EStage
            F32 mAge01 = 0.f;
            F64 mBirthTime = 0.0;
            F32 mLifetimeS = 0.f;
            F32 mRotation = 0.f;     // signed Omega
            F32 mMeso = 0.f;         // lifecycle rotation strength (0 for non-supercells)
            F32 mIntensity = 0.f;
            bool mSupercell = false;
            bool mIsHero = false;
            // <SS:Nexii> SQUALL/FORCED (doc/atmo_magic_storm_dynamics.md sections 5-6): mirrors ActiveCell's own
            // mLineId/mIsForced straight across (SSStormCells::ActiveCell) - 0 is never a real line id (see
            // ssstormcellcore.h's own comment on ActiveCell::mLineId), so a plain != 0 test is "this cell is a
            // squall-line member". mIsForced is the (at most one) authored/forced pin (SSSquall::forcedCandidate).
            U64 mLineId = 0;
            bool mIsForced = false;
        };
        struct HeroPath
        {
            U64 mId = 0;
            LLVector2 mOriginAgent;
            LLVector2 mNowAgent;
            LLVector2 mDeathAgent;
            LLVector2 mClosestAgent;
            LLVector2 mMotion;       // m/s, agent axes == global axes
            F32 mClosestDistM = 0.f;
            F64 mBirthTime = 0.0;
            F64 mClosestTime = 0.0;
            F32 mLifetimeS = 0.f;
            F32 mRotation = 0.f;
        };

        // <SS:Nexii> DEBUG: one entry per SSVortices::LiveVortex (active(), <= SSVortex::MAX_ACTIVE, already ranked
        // hero-first by the scheduler). mKind is the DISPLAY kind (post waterspout relabel) as an
        // SSAtmoInfoViewCore::VORTEX_KIND_* value; mCollars is only populated (SSVortex::COLLARS entries) when
        // mHasFunnel - a funnel-less gustnado carries an empty table. [interaction: SSVortices active]
        struct VortexIcon
        {
            struct Collar
            {
                F32 mRadiusM = 0.f;
                F32 mAltitudeZ = 0.f;   // world Z (already the collar table's own frame - see SSVortex::collarAltitudeM)
            };

            U64 mId = 0;
            S32 mKind = 0;             // SSAtmoInfoViewCore::VORTEX_KIND_*
            F32 mRotationSign = 1.f;
            bool mParentIsHero = false;
            bool mWasRelabelledWaterspout = false;
            LLVector2 mContactAgent;
            F32 mCondensation = 0.f;   // [0,1] funnel-aloft -> touchdown -> rope-out
            F32 mIntensity = 0.f;      // live intensity
            S32 mMultiN = 0;           // 0 = single vortex; else suction-vortex count
            bool mHasFunnel = false;
            std::vector<Collar> mCollars;
        };

        // A live dust devil (SSVortices::dustDevils()) - parentless, no funnel/collar table.
        struct DustIcon
        {
            U64 mId = 0;
            LLVector2 mOriginAgent;
            F32 mIntensity = 0.f;
        };

        // <SS:Nexii> DEBUG: the tornado "why not" readout (SSVortices::WhyNotTornado), keyed to the SAME hero V2
        // already shows - this answers "why does the hero cell that DOES exist not carry a live tornado-family
        // funnel right now", distinct from mWhyNot below (which answers "why is there no hero cell at all").
        // mKind is slot 0's decided SSAtmoInfoViewCore::VORTEX_KIND_* (VORTEX_KIND_NONE if no gate passed at all).
        // Empty mFailing iff mAlive. [interaction: SSVortices whyNot]
        struct TornadoWhyNot
        {
            bool mHaveHero = false;
            S32 mKind = 0;
            bool mAlive = false;
            F32 mMeso = 0.f;
            F32 mPotential = 0.f;
            bool mSupercell = false;
            bool mTornadoEligible = false;
            bool mAllowTornadoes = false;
            std::vector<std::string> mFailing;
        };

        // <SS:Nexii> V2 SQUALL (doc/atmo_magic_storm_dynamics.md section 5, V2's own "bar through the members"
        // ask): one entry per DISTINCT active mLineId this frame, reconstructed read-only through
        // SSSquall::lineEvent/lineMember/qlcsJunctionAt (pure functions of seed/epoch/anchor/windAnvil/severe, all
        // re-read from the cube at the SCHEDULER's phase - 7c) rather than a new field carried
        // on the scheduler. mMembersAgent is only the members that are ACTUALLY alive+spawned this frame (in the
        // template's own along-the-line order, which the offset formula makes monotone in member index), so the
        // bar always matches the rings V2 already draws; mJunctionsAgent is every inter-member leading-edge QLCS
        // spin-up point the template defines, drawn whether or not its neighbours happen to be alive (a property of
        // the line's geometry, not of which members gated this instant).
        struct SquallLine
        {
            U64 mLineId = 0;
            std::vector<LLVector2> mMembersAgent;
            std::vector<LLVector2> mJunctionsAgent;
        };

        // <SS:Nexii> V2 LINE BAND (doc/atmo_magic_phase8_show.md section 3 item 1): mirrors SSStormCells::
        // fillLineBand's own SSStormCouple::LineBand field for field, straight off the SAME closed-form band the
        // deck coupling itself reads (ssstormcouplecore.h's own comment) - never re-derived here. mOriginAgent is
        // ALREADY agent frame (fillLineBand's own toAgentXY conversion); mDir/mMotion are unit vectors (mMotion is
        // (0,0) when the line is stalled); mHalfLenM/mBandM/mShelfM are metres; mStrength <= 0 means no line is
        // alive this frame (SSStormCouple::lineField's own "disabled" reading) and the drawing code skips it.
        struct LineBand
        {
            LLVector2 mOriginAgent;
            LLVector2 mDir;
            LLVector2 mMotion;
            F32 mHalfLenM = 0.f;
            F32 mBandM = 0.f;
            F32 mShelfM = 0.f;
            F32 mStrength = 0.f;
        };

        bool mValid = false;
        bool mDeckBuilt = false;
        bool mHaveHero = false;
        bool mAllowSupercells = false;   // the applied track's live checkboxes
        bool mAllowTornadoes = false;
        F64 mNow = 0.0;
        F32 mLayerZ = 0.f;               // altitude the layer is drawn at: the deck base when built, else the drift base
        LLVector2 mAnchorAgent;
        S32 mCandidates = 0;
        S32 mAlive = 0;
        S32 mSpawned = 0;
        S32 mSupercells = 0;
        S32 mTornadoEligible = 0;
        std::vector<Tile> mTiles;
        std::vector<Cell> mCells;        // ranked by hashed id, as the scheduler left them
        std::vector<SquallLine> mSquallLines;
        LineBand mLineBand;
        HeroPath mHero;
        std::vector<std::string> mWhyNot;
        std::vector<VortexIcon> mVortices;     // empty when SSVortices is not valid this frame
        std::vector<DustIcon> mDustDevils;
        TornadoWhyNot mVortexWhyNot;
    };
    static StormCellsData stormCellsData();

    // <SS:Nexii> V3's data (doc/atmo_magic_far_clouds.md LOD phase, ssdecklodcore.h CONTRACT): a distance-only
    // diagram of the deck LOD ramp, not a replay of the builder's own cell gate/occupancy - this view has no
    // access to that state and must not reimplement it (the gate stays exactly where it is). mDeckBuilt mirrors
    // SSVolCloud::empty(); the counts are the primary deck's own last build (mPuffsPlaced is the COMBINED
    // primary+under count SSVolCloud::puffCount() already reports - the budget dial is shared across both decks).
    // mTierBCount added for LOD phase 6d (ssdeckmacrocore.h CONTRACT, doc/atmo_magic_far_clouds.md section 2 step
    // 2 Tier B): the primary deck's own last-build count of merged macro-puff bodies (SSVolCloud::
    // primaryTierBCount(), itself Deck::mTierBCount read straight across) - included in mPuffsPlaced already
    // (Tier B bodies are ordinary Puffs), broken out separately here only for the legend's own "Tier B" line.
    struct DeckLodData
    {
        bool mDeckBuilt = false;
        F32 mLayerZ = 0.f;
        S32 mPuffsPlaced = 0;
        S32 mBudget = 0;
        S32 mPuffsPerCell = 0;
        S64 mLodPredicted = 0;
        S32 mCellsWalked = 0;
        S32 mTierBCount = 0;
    };
    static DeckLodData deckLodData();

    // <SS:Nexii> V4's data (doc/atmo_magic_debug_views.md V4, ssvirgacore.h CONTRACT): the last build's virga
    // snapshot straight off SSVolCloud::virgaDebug() (read-only - this view never re-derives which cells qualify
    // or how the hashed trim ordered them), plus the wind and fall-speed numbers needed to draw the fall-tilt
    // comparison line (SSAtmoInfoViewCore::fallTiltOffsetM): the SAME ground-level wind the applier's profile
    // gives (SSWindProfile::windAt(0, params) - a display approximation of the flowmap-advected wind
    // ssprecipitation.cpp's own spawner actually uses, close enough for a comparison line, never claimed
    // bit-identical to it) and the active precip preset's fall speed. mValid mirrors windProfileData()'s own
    // "is a weather cube even applied" gate; mActive/mDeckBuilt mirror the snapshot and SSVolCloud::empty().
    // [interaction: SSVolCloud virgaDebug] [interaction: SSAtmoMagic preset/windXY] [interaction: SSAtmoEnvApplier windProfile]
    struct VirgaData
    {
        struct Cell
        {
            F32 mX = 0.f;
            F32 mY = 0.f;
            F32 mDrive = 0.f;
            bool mKept = false;
        };
        bool mValid = false;
        bool mActive = false;
        bool mDeckBuilt = false;
        F32 mGroundZ = 0.f;
        F32 mBaseZ = 0.f;
        F32 mR2 = 0.f;              // the particle rain's own TIER_SHEETS radius
        F32 mHandoffRadius = 0.f;   // r2 * SSVirga::HANDOFF_SKIP
        F32 mFallSpeedMS = 0.f;
        SSWindProfile::Vec2 mWindGround;
        // <SS:Nexii> F9 (2026-09-06 review), 8e-b PROFILE SKEW: SSVolCloud::virgaDebug()'s own snapshot of the
        // emitter's skew inputs (SSVirgaDebug::mWindParams/mBaseAglM/mFallSpeed/mEmbedTopZ), copied straight
        // across like mCells' mKept flags - never re-derived, so the drawn skew polyline can never disagree with
        // the geometry it describes. mWindParams/mBaseAglM are the curve-resolved wind PROFILE and the deck
        // base's own AGL (not mWindGround's live, per-client single-altitude approximation at z=0) - the pair
        // profileSkewM needs to reconstruct a card's own chord; mFallSpeed is the same active preset value as
        // mFallSpeedMS, kept as its own field because it belongs to a different comparison line (see renderVirga's
        // own comment on the two lines' distinct purposes).
        SSWindProfile::Params mWindParams;
        F32 mBaseAglM = 0.f;
        F32 mFallSpeed = 0.f;
        F32 mEmbedTopZ = 0.f;      // SSVirga::embedTopZ(mBaseZ, deck thickness) - where the card stack itself starts
        S32 mCandidates = 0;
        S32 mKept = 0;
        S32 mTrimmed = 0;
        std::vector<Cell> mCells;
    };
    static VirgaData virgaData();

    // <SS:Nexii> V5's data (doc/atmo_magic_debug_views.md V5): the applied track's day-cycle cube sampled at a
    // fixed grid of phases through the SAME resolvers the sky itself reads (SSAtmoEnvWeatherResolver::resolve,
    // SSAtmoEnvCloudFieldResolver::resolve, SSWindProfile::consolidation, SSStormCell::gateScore) - nothing here is
    // a second-guessed formula, every number is a pure function call on published inputs. mSamples is one row per
    // grid point (phase 0..1 inclusive so the day-cycle wrap closes visually); mCues is the authored override
    // keyframes (mStormOverride/mPrecipitationOverride) whose value is active, at THEIR OWN keyframe phase - the
    // markers V5 asks for. mNowPhase is the SAME applied phase the sky itself is drawn at
    // (SSAtmoEnvApplier::appliedPhase, carrying the editor's preview override when one is set), not a fresh clock
    // read, so the "now" cursor never disagrees with the sky beside it. Storm spawn score at the anchor
    // (SSStormCell::gateScore) is read at the anchor's own lattice cell and the CURRENT epoch only (never swept per
    // sample - an epoch is a wall-clock bucket, not a day-cycle phase) with just the weather term varied per
    // sample, answering "what would this candidate's score be if it were born at this time of day"; mHaveStormScore
    // is true whenever mValid is (SSStormCells::anchorNow() is trusted directly once a weather cube is actually
    // applied - see weatherCubeData()'s own comment), reserved as its own flag so a future gate on it never touches
    // every other field's own meaning. Read-only: no accessor here builds anything or advances any clock. [interaction: SSAtmoEnvWeatherResolver] [interaction:
    // SSAtmoEnvCloudFieldResolver] [interaction: SSStormCell::gateScore]
    struct WeatherCubeData
    {
        struct Sample
        {
            F32 mPhase = 0.f;
            F32 mMoisture = 0.f;
            F32 mConvection = 0.f;
            F32 mTemperatureC = 0.f;
            F32 mWindSpeed = 0.f;
            F32 mWindHeading = 0.f;
            F32 mShearStrength = 0.f;
            F32 mVeerDeg = 0.f;
            F32 mConsolidation = 0.f;
            F32 mGloom = 0.f;
            F32 mAnvilRamp = 0.f;
            F32 mLightningIntensity = 0.f;
            bool mLightningActive = false;
            F32 mStormScore = 0.f;
        };
        struct Cue
        {
            F64 mPhase = 0.0;
            std::string mLabel;
            bool mIsStorm = false; // true: mStormOverride cue, false: mPrecipitationOverride cue
        };

        bool mValid = false;
        bool mHaveStormScore = false;
        std::string mTrackName;
        F64 mDayLengthS = 0.0;
        F64 mNowPhase = 0.0;
        std::vector<Sample> mSamples;
        std::vector<Cue> mCues;
    };
    static WeatherCubeData weatherCubeData();

    // <SS:Nexii> V9's data (doc/atmo_magic_debug_views.md V9): the live strike list exactly as SSLightning::idle()
    // left it - one row per SSStrike in SSLightning::strikes(), scalars only. The CHANNEL GEOMETRY is deliberately
    // NOT copied here: a channel runs to hundreds of nodes and there can be a dozen strikes at once, so
    // renderLightning() walks SSLightning::strikes() directly (a const reference to resident state, read the same
    // frame) while this struct carries only what the legend and the chart need. Every field is a straight read of
    // a member the model already maintains; mStage is SSAtmoInfoViewCore::strikeStage of that strike's own clock,
    // leader and plasma fields, and mLightWeight/mLightsCloud are the core's reading of the SAME two numbers
    // ssvolcloud.cpp's uploader takes (channel brightness x intensity against SS_MAX_STRIKE_LIGHTS' own cut) -
    // a reading of the shared inputs, not the uploaded array, which SSVolCloud keeps private. mLights IS the real
    // thing: SSLightning::sceneLights(), the const exporter the deferred lighting pass itself calls, asked for the
    // same 4 slots pipeline.cpp asks for. mValid is false when no SSLightning instance exists; the gate fields
    // mirror the applied weather's own lightning row so "nothing is striking" can be read without guessing.
    // Read-only by construction: every accessor touched here is a const getter, and nothing here advances a clock.
    // [interaction: SSLightning strikes/nextStrikeIn/sceneLights] [interaction: SSAtmoMagic lightning row]
    // [interaction: SSVolCloud SS_MAX_STRIKE_LIGHTS]
    struct LightningData
    {
        struct Strike
        {
            S32 mKind = 0;             // SSAtmoInfoViewCore::STRIKE_KIND_*
            S32 mStage = 0;            // SSAtmoInfoViewCore::STRIKE_STAGE_*
            F32 mT = 0.f;              // the strike's own clock: negative before contact
            F32 mIntensity = 1.f;
            F32 mCharge = 0.f;
            F32 mChargeHeld = 0.f;
            F32 mLeaderProgress = 0.f;
            F32 mChannelBrightness = 0.f;
            F32 mFlash = 0.f;
            F32 mHit = 0.f;            // amber impact flare envelope
            F32 mFire = 0.f;           // ground fire envelope
            F32 mPlasmaSince = -1.f;
            S32 mStrokeCount = 0;
            S32 mChannelNodes = 0;
            S32 mCrawlCount = 0;
            F32 mCrawlLenM = 0.f;
            F32 mChannelLenM = 0.f;
            F32 mDistanceM = 0.f;
            F32 mSteamPeak = 0.f;
            bool mPositive = false;    // polarity: a positive anvil discharge
            bool mBlue = false;        // bolt from the blue
            bool mForced = false;      // placed by the Strike Now / Ground Strike buttons
            bool mAudible = false;
            bool mOccHidden = false;   // the renderer's own occlusion query said the ground show is hidden
            F32 mLightWeight = 0.f;    // channel brightness x intensity
            bool mLightsCloud = false; // ... clears the cloud shader's own cut
            LLVector3 mOrigin;
            LLVector3 mGround;
        };

        // One deferred point light as SSLightning::sceneLights() exports it (channel lights first, then the
        // ground fire's amber light) - position, radius, and the colour already scaled by the strike's own
        // brightness and the scene-light strength dial.
        struct SceneLight
        {
            LLVector3 mPos;
            F32 mRadiusM = 0.f;
            LLColor3 mColor;
        };

        bool mValid = false;
        bool mEnabled = false;         // SSAtmoMagic::lightningOn() - the applied weather's own switch
        bool mChargeOn = false;
        bool mSparksOn = false;
        F32 mIntensity = 0.f;          // the weather row's lightning intensity (0 = no strikes scheduled)
        F32 mIntervalMinS = 0.f;
        F32 mIntervalMaxS = 0.f;
        F64 mNextIn = -1.0;            // seconds to the next scheduled strike (negative: none scheduled)
        S32 mCloudLit = 0;             // strikes clearing the cloud shader's cut this frame
        S32 mCloudCap = 0;             // SS_MAX_STRIKE_LIGHTS
        S32 mSceneLightCap = 0;        // the slot count pipeline.cpp asks sceneLights for
        std::vector<Strike> mStrikes;
        std::vector<SceneLight> mLights;
    };
    static LightningData lightningData();

    // <SS:Nexii> V10's data (doc/atmo_magic_acoustics.md, the debug ask): the world
    // field's baked acoustic channel read straight off the tile - probes and links
    // in range of the camera (SSWorldField::acousticDebug, read-only over the bake),
    // the listener's own blend set, the soundscape's last thunder with its
    // propagation figures (SSSoundscape::lastThunderPath) - plus the summary scalars
    // the legend prints. mValid is false with no current bake (no tile, stale serial,
    // channel off); the layer then says so instead of drawing yesterday's room.
    // Read-only by construction: every accessor touched here is a const getter, and
    // nothing here re-solves the propagation or re-bakes anything.
    // [interaction: SSWorldField acousticDebug] [interaction: SSSoundscape lastThunderPath]
    struct AcousticsData
    {
        bool mValid = false;        // a current probe bake exists for the camera's region
        bool mEnabled = false;      // SSWorldFieldAcoustics
        U32 mQuality = 0;           // SSWorldFieldAcousticsQuality (0 tier A, 1 tier B)
        S32 mProbeCount = 0;        // the tile's full probe count
        S32 mBundleCount = 0;       // probes carrying a tier B bundle
        S32 mLinkCount = 0;
        S32 mPortalCount = 0;
        F32 mLatCell = 0.f;
        SSWorldField::AcousticDebug mField;
        SSSoundscape::ThunderPath mThunder; // the last scheduled thunder's propagation
        F64 mThunderAge = -1.0;             // seconds since it was scheduled; -1 none
    };
    static AcousticsData acousticsData();

private:
    static void onModeChanged(U32 previous, U32 now);
    // <SS:Nexii> The info-view LOOK: one full-screen triangle (gSSInfoLookProgram, ssInfoLookV/F.glsl) that overwrites the world viewport with a warm-gray reading of the frame that was just presented - luminance on a clay-to-cream ramp, a 12-tap hemisphere occlusion recomputed from the G-buffer, a soft sky-lit shade and an extreme-fog distance ramp, sky flat. The formulas are SSInfoLook (ssinfolookcore.h); the shader transliterates them and V:\Scratch\atmo\tests\twin_infolook.cpp holds the two together. Gated by SSAtmoInfoViewLook (default on), which replaced the old SSAtmoInfoViewDim slider. Called only from renderDimAndWorld(), before renderWorld(). [interaction: LLPipeline::mSSLastPresented]
    static void renderInfoLook();
    static void renderWindMast();
    static void renderStormCells();
    static void renderDeckLod();
    static void renderVirga();
    static void renderAcoustics();
    // <SS:Nexii> V9's in-world layer. Called for MODE_LIGHTNING and ALSO whenever RENDER_DEBUG_LIGHTNING is set, which is what makes it the engineering overlay bound beside the other seven on the debug floater as well as an info view; renderWorld() draws it exactly once either way. [interaction: LLPipeline::RENDER_DEBUG_LIGHTNING]
    static void renderLightning();

    static U32 sLastMode;
    static bool sFlowMaskWasOn;
    static boost::signals2::scoped_connection sModeConnection;
};

// <SS:Nexii> Vestigial. This class once WAS the world dimmer - one translucent dark quad over the debug-view rect, alpha from SSAtmoInfoViewDim, drawn as the debug view's first child. The quad moved into the 3-D pass (SSAtmoInfoView::renderDimAndWorld) because the in-world layer it dimmed has to draw on top of it, and then the quad itself was replaced outright by the info-view LOOK pass (renderInfoLook) after the user's verdict that dimming "is just making the world black at max". Nothing about a dim survives: there is no SSAtmoInfoViewDim setting and no draw here. The class stays so LLDebugView's addChildInBack and the debug view's child order are untouched.
class SSAtmoDimView : public LLView
{
public:
    // <SS:Nexii> draw() is deliberately empty: the dim quad moved into the 3-D pass (SSAtmoInfoView::renderDimAndWorld, called from render_ui() in llviewerdisplay.cpp, post-tonemap, between renderFinalize and the HUD passes) because the in-world layer it dims has to draw on top of it, and a 3-D layer re-entered from the UI stage is what clipped the consoles and threw the visualisations into the corners. The class stays so LLDebugView's addChildInBack and the debug view's child order are untouched.
    struct Params : public LLInitParam::Block<Params, LLView::Params>
    {
        Params()
        {
            changeDefault(mouse_opaque, false);
        }
    };

    SSAtmoDimView(const Params& p);
    void draw() override;
};

// <SS:Nexii> The one shared legend (SSStatsView idiom: translucent, monospace, read-only, sized to content), docked bottom-left of the debug view: mode title, a gradient bar with min/max labels in real units, and the mode's icon key. Every mode pushes rows through the same interface; none invents its own panel.
class SSAtmoLegendView : public LLView
{
public:
    struct Params : public LLInitParam::Block<Params, LLView::Params>
    {
        Params()
        {
            changeDefault(mouse_opaque, false);
        }
    };

    struct KeyRow
    {
        LLColor4 mColor;
        std::string mLabel;
        bool mSwatchIsLine = false;
    };

    struct Spec
    {
        std::string mTitle;
        std::string mSubtitle;
        std::string mRampLabel;   // what the gradient bar measures, e.g. "wind speed"
        std::string mRampMin;     // left-end label in real units
        std::string mRampMax;     // right-end label in real units
        // Ramp sampler: unit t in [0,1] -> colour. Null hides the bar.
        LLColor4 (*mRamp)(F32 t) = nullptr;
        std::vector<KeyRow> mKeys;
    };

    SSAtmoLegendView(const Params& p);
    void draw() override;

private:
    static void buildWindProfileSpec(Spec& spec);
    static void buildStormCellsSpec(Spec& spec);
    static void buildDeckLodSpec(Spec& spec);
    static void buildVirgaSpec(Spec& spec);
    static void buildWeatherCubeSpec(Spec& spec);
    static void buildLightningSpec(Spec& spec);
    static void buildAcousticsSpec(Spec& spec);
};

// <SS:Nexii> The reusable chart widget (XUI tag ss_atmo_graph_view, still registered for any future XUI use, though the debug HUD's own instance is now built programmatically by SSAtmoInfoView::attach rather than parsed off panel_ss_atmo_debug_views.xml): an LLView drawing polylines, rails and monospace labels with gGL in 2D. Dispatches on the live mode - V1 draws the altitude-vs-speed boundary-layer curve with annotated rails and a hodograph inset; V2 the active-cell timeline (one row per cell, stage bands, the now cursor, hero row pinned on top); V3 the deck LOD count-vs-budget chart; V4 the shaft budget bar, the drive histogram and the particle-rain handoff ramp; V5 the two-lane day-cycle chart (authored curves above, derived gates below, a now cursor and authored-cue markers spanning both); V9 the strike timeline. draw() draws nothing at all - not even the background quad - when mode() is MODE_OFF or the live mode has no chart (today only the RESERVED numbers V6 World Field and V8 Anatomy, neither of which is built; decided by falling through the SAME switch that dispatches to drawWindProfile/drawStormCells/drawDeckLod/drawVirga/drawWeatherCube/drawLightning below, not a separately maintained mode list) so the debug HUD never shows an empty placeholder box floating over the world. When it does have something to draw it first re-docks itself against the legend (see SSAtmoInfoView::attach), fixed at ~440x280 px, left edge 8px past the legend's right edge, bottom aligned with the legend's bottom; FOLLOWS_BOTTOM|FOLLOWS_LEFT keeps that pair anchored together through a window resize exactly like the legend anchors itself. Read-only like everything else here; it never asks any system to build.
class SSAtmoGraphView : public LLView
{
public:
    struct Params : public LLInitParam::Block<Params, LLView::Params>
    {
        Params()
        {
            changeDefault(mouse_opaque, false);
        }
    };

    SSAtmoGraphView(const Params& p);
    void draw() override;

private:
    void drawWindProfile();
    void drawStormCells();
    void drawDeckLod();
    void drawWeatherCube();
    void drawVirga();
    void drawLightning();
    void drawMessage(const std::string& msg);

    // 2D primitives in local view pixels.
    static void polyline(const std::vector<std::pair<S32, S32> >& pts, const LLColor4& color, bool dashed = false);
    static void text(const std::string& s, S32 x, S32 y, const LLColor4& color, bool right_align = false);
};

#endif
